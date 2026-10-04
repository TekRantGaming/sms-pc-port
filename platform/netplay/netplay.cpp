// Online co-op: UDP host/join and pose relay (see netplay.h).
//
// Packets start with a 6-byte header: magic "SMSN", protocol version, type.
//   HELLO   client -> host   name
//   WELCOME host -> client   the client's slot
//   REJECT  host -> client   reason (lobby full, other version)
//   POSE    client -> host   sequence number, NetPose
//   STATE   host -> client   every other connected player: slot, name, NetPose
//   BYE     either way       leaving
// Poses go out about 60 times a second; a player silent for 5 s is dropped.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "netplay.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" void port_log(const char* fmt, ...);

namespace {

#ifdef _WIN32
typedef SOCKET Socket;
const Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
typedef int Socket;
const Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

const uint32_t kMagic = 0x4E534D53; // "SMSN"
const uint8_t kVersion = 1;
enum Type : uint8_t { HELLO = 1, WELCOME, REJECT, POSE, STATE, BYE };
enum Reject : uint8_t { REJECT_FULL = 1, REJECT_VERSION = 2 };
const double kTimeout = 5.0, kSendInterval = 1.0 / 60.0;

double now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#pragma pack(push, 1)
struct Header {
	uint32_t magic;
	uint8_t version, type;
};
struct StateEntry {
	uint8_t slot;
	char name[NET_NAME_LEN];
	NetPose pose;
};
#pragma pack(pop)

struct Peer {
	bool active = false;
	sockaddr_storage addr{};
	socklen_t addrLen = 0;
	char name[NET_NAME_LEN] = {};
	NetPose pose{};
	uint32_t seq = 0;
	double lastSeen = 0, poseAt = 0;
};

struct Net {
	enum Mode { OFF, HOST, JOIN } mode = OFF;
	std::mutex mu;
	Socket sock = kNoSocket;
	std::thread thread;
	std::atomic<bool> quit{false};
	std::string address, status = "offline";
	int port = 27016;
	char name[NET_NAME_LEN] = {};
	NetPose local{};
	bool haveLocal = false;
	uint32_t localSeq = 0;
	// host: peers[0] is the host itself; join: the other players by slot
	Peer peers[NET_MAX_PLAYERS];
	int mySlot = -1;
	sockaddr_storage hostAddr{};
	socklen_t hostLen = 0;
	double hostSeen = 0, lastHello = 0;

	void sendTo(const void* data, size_t n, const sockaddr_storage& to, socklen_t len)
	{
		sendto(sock, static_cast<const char*>(data), int(n), 0, reinterpret_cast<const sockaddr*>(&to), len);
	}
	void sendSimple(uint8_t type, const void* body, size_t n, const sockaddr_storage& to, socklen_t len)
	{
		char buf[256];
		Header h = { kMagic, kVersion, type };
		memcpy(buf, &h, sizeof h);
		if (n)
			memcpy(buf + sizeof h, body, n);
		sendTo(buf, sizeof h + n, to, len);
	}

	static bool sameAddr(const sockaddr_storage& a, socklen_t al, const sockaddr_storage& b, socklen_t bl)
	{
		return al == bl && memcmp(&a, &b, al) == 0;
	}

	bool openSocket(bool bind_)
	{
		sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == kNoSocket)
			return false;
#ifdef _WIN32
		u_long nb = 1;
		ioctlsocket(sock, FIONBIO, &nb);
		// an ICMP "port unreachable" must not make recvfrom fail forever
		BOOL off = FALSE;
		DWORD ret = 0;
		WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &ret,
		         nullptr, nullptr);
#else
		fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
		if (bind_) {
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_addr.s_addr = htonl(INADDR_ANY);
			a.sin_port = htons(uint16_t(port));
			if (bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
				closeSocket(sock);
				sock = kNoSocket;
				return false;
			}
		}
		return true;
	}

	void start()
	{
		const char* m = getenv("SMS_NET_MODE");
		mode = !m ? OFF : !strcmp(m, "host") ? HOST : !strcmp(m, "join") ? JOIN : OFF;
		if (mode == OFF)
			return;
		if (const char* p = getenv("SMS_NET_PORT"))
			port = atoi(p) > 0 && atoi(p) < 65536 ? atoi(p) : 27016;
		const char* n = getenv("SMS_NET_NAME");
		snprintf(name, sizeof name, "%s", n && *n ? n : "Mario");
		if (const char* a = getenv("SMS_NET_ADDRESS"))
			address = a;
#ifdef _WIN32
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			status = "network unavailable";
			mode = OFF;
			return;
		}
#endif
		if (mode == HOST) {
			if (!openSocket(true)) {
				char buf[96];
				snprintf(buf, sizeof buf, "cannot host: UDP port %d is in use", port);
				status = buf;
				port_log("[net] %s\n", buf);
				mode = OFF;
				return;
			}
			peers[0].active = true;
			memcpy(peers[0].name, name, sizeof name);
			mySlot = 0;
			port_log("[net] hosting on UDP port %d as %s\n", port, name);
		} else {
			addrinfo hints{}, *res = nullptr;
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			char portStr[16];
			snprintf(portStr, sizeof portStr, "%d", port);
			if (address.empty() || getaddrinfo(address.c_str(), portStr, &hints, &res) != 0 || !res) {
				status = "cannot find the host " + address;
				port_log("[net] %s\n", status.c_str());
				mode = OFF;
				return;
			}
			memcpy(&hostAddr, res->ai_addr, res->ai_addrlen);
			hostLen = socklen_t(res->ai_addrlen);
			freeaddrinfo(res);
			if (!openSocket(false)) {
				status = "network unavailable";
				mode = OFF;
				return;
			}
			port_log("[net] joining %s:%d as %s\n", address.c_str(), port, name);
		}
		thread = std::thread([this] { loop(); });
	}

	void loop()
	{
		double lastSend = 0;
		while (!quit) {
			receive();
			const double t = now();
			if (t - lastSend >= kSendInterval) {
				lastSend = t;
				std::lock_guard<std::mutex> lk(mu);
				if (mode == HOST)
					hostTick(t);
				else
					joinTick(t);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		std::lock_guard<std::mutex> lk(mu);
		if (mode == JOIN && mySlot >= 0)
			sendSimple(BYE, nullptr, 0, hostAddr, hostLen);
		for (int i = 1; i < NET_MAX_PLAYERS && mode == HOST; i++)
			if (peers[i].active)
				sendSimple(BYE, nullptr, 0, peers[i].addr, peers[i].addrLen);
	}

	void receive()
	{
		char buf[2048];
		for (;;) {
			sockaddr_storage from{};
			socklen_t fromLen = sizeof from;
			const int n = int(recvfrom(sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
			if (n < int(sizeof(Header)))
				return;
			Header h;
			memcpy(&h, buf, sizeof h);
			if (h.magic != kMagic)
				continue;
			std::lock_guard<std::mutex> lk(mu);
			if (h.version != kVersion) {
				if (mode == HOST) {
					uint8_t r = REJECT_VERSION;
					sendSimple(REJECT, &r, 1, from, fromLen);
				}
				continue;
			}
			const char* body = buf + sizeof h;
			const int len = n - int(sizeof h);
			if (mode == HOST)
				hostPacket(h.type, body, len, from, fromLen);
			else if (sameAddr(from, fromLen, hostAddr, hostLen))
				joinPacket(h.type, body, len);
		}
	}

	// --- host
	int peerFor(const sockaddr_storage& a, socklen_t l)
	{
		for (int i = 1; i < NET_MAX_PLAYERS; i++)
			if (peers[i].active && sameAddr(peers[i].addr, peers[i].addrLen, a, l))
				return i;
		return -1;
	}
	void hostPacket(uint8_t type, const char* body, int len, const sockaddr_storage& from, socklen_t fromLen)
	{
		int slot = peerFor(from, fromLen);
		const double t = now();
		if (type == HELLO) {
			if (slot < 0) {
				for (int i = 1; i < NET_MAX_PLAYERS && slot < 0; i++)
					if (!peers[i].active)
						slot = i;
				if (slot < 0) {
					uint8_t r = REJECT_FULL;
					sendSimple(REJECT, &r, 1, from, fromLen);
					return;
				}
				Peer& p = peers[slot];
				p = Peer();
				p.active = true;
				p.addr = from;
				p.addrLen = fromLen;
				memcpy(p.name, body, len >= NET_NAME_LEN ? NET_NAME_LEN : size_t(len > 0 ? len : 0));
				p.name[NET_NAME_LEN - 1] = 0;
				port_log("[net] %s joined (slot %d)\n", p.name, slot);
			}
			peers[slot].lastSeen = t;
			uint8_t s = uint8_t(slot);
			sendSimple(WELCOME, &s, 1, from, fromLen);
			return;
		}
		if (slot < 0)
			return;
		Peer& p = peers[slot];
		p.lastSeen = t;
		if (type == POSE && len >= int(sizeof(uint32_t) + sizeof(NetPose))) {
			uint32_t seq;
			memcpy(&seq, body, sizeof seq);
			if (int32_t(seq - p.seq) > 0 || p.poseAt == 0) {
				p.seq = seq;
				memcpy(&p.pose, body + sizeof seq, sizeof(NetPose));
				p.poseAt = t;
			}
		} else if (type == BYE) {
			port_log("[net] %s left\n", p.name);
			p.active = false;
		}
	}
	void hostTick(double t)
	{
		if (haveLocal) {
			peers[0].pose = local;
			peers[0].poseAt = t;
		}
		int count = 1;
		for (int i = 1; i < NET_MAX_PLAYERS; i++) {
			if (peers[i].active && t - peers[i].lastSeen > kTimeout) {
				port_log("[net] %s timed out\n", peers[i].name);
				peers[i].active = false;
			}
			count += peers[i].active;
		}
		char buf[sizeof(Header) + 1 + NET_MAX_PLAYERS * sizeof(StateEntry)];
		for (int to = 1; to < NET_MAX_PLAYERS; to++) {
			if (!peers[to].active)
				continue;
			Header h = { kMagic, kVersion, STATE };
			memcpy(buf, &h, sizeof h);
			uint8_t n = 0;
			char* out = buf + sizeof h + 1;
			for (int i = 0; i < NET_MAX_PLAYERS; i++) {
				if (i == to || !peers[i].active || peers[i].poseAt == 0)
					continue;
				StateEntry e;
				e.slot = uint8_t(i);
				memcpy(e.name, peers[i].name, NET_NAME_LEN);
				e.pose = peers[i].pose;
				memcpy(out, &e, sizeof e);
				out += sizeof e;
				n++;
			}
			buf[sizeof h] = char(n);
			sendTo(buf, size_t(out - buf), peers[to].addr, peers[to].addrLen);
		}
		char s[96];
		snprintf(s, sizeof s, "Hosting on port %d: %d player%s", port, count, count == 1 ? "" : "s");
		status = s;
	}

	// --- join
	void joinPacket(uint8_t type, const char* body, int len)
	{
		const double t = now();
		hostSeen = t;
		if (type == WELCOME && len >= 1) {
			if (mySlot < 0)
				port_log("[net] joined %s as slot %d\n", address.c_str(), uint8_t(body[0]));
			mySlot = uint8_t(body[0]);
		} else if (type == REJECT && len >= 1) {
			status = body[0] == REJECT_FULL ? "The game is full" : "The host runs a different version";
			port_log("[net] rejected: %s\n", status.c_str());
		} else if (type == STATE && len >= 1) {
			const int n = uint8_t(body[0]);
			bool seen[NET_MAX_PLAYERS] = {};
			for (int i = 0; i < n && 1 + (i + 1) * int(sizeof(StateEntry)) <= len; i++) {
				StateEntry e;
				memcpy(&e, body + 1 + i * sizeof(StateEntry), sizeof e);
				if (e.slot >= NET_MAX_PLAYERS || e.slot == mySlot)
					continue;
				Peer& p = peers[e.slot];
				if (!p.active)
					port_log("[net] %.*s is here\n", NET_NAME_LEN, e.name);
				p.active = true;
				memcpy(p.name, e.name, NET_NAME_LEN);
				p.name[NET_NAME_LEN - 1] = 0;
				p.pose = e.pose;
				p.poseAt = t;
				seen[e.slot] = true;
			}
			for (int i = 0; i < NET_MAX_PLAYERS; i++)
				if (!seen[i] && peers[i].active && t - peers[i].poseAt > 2.0) {
					port_log("[net] %s left\n", peers[i].name);
					peers[i].active = false;
				}
		} else if (type == BYE) {
			port_log("[net] the host closed the game\n");
			mySlot = -1;
			for (Peer& p : peers)
				p.active = false;
		}
	}
	void joinTick(double t)
	{
		if (mySlot >= 0 && t - hostSeen > kTimeout) {
			port_log("[net] lost the connection to the host\n");
			mySlot = -1;
			for (Peer& p : peers)
				p.active = false;
		}
		if (mySlot < 0) {
			if (t - lastHello > 0.5) {
				lastHello = t;
				sendSimple(HELLO, name, NET_NAME_LEN, hostAddr, hostLen);
			}
			if (status.compare(0, 8, "The game") != 0 && status.compare(0, 8, "The host") != 0)
				status = "Joining " + address + "...";
			return;
		}
		char buf[sizeof(uint32_t) + sizeof(NetPose)];
		const uint32_t seq = ++localSeq;
		NetPose pose = haveLocal ? local : NetPose();
		memcpy(buf, &seq, sizeof seq);
		memcpy(buf + sizeof seq, &pose, sizeof pose);
		sendSimple(POSE, buf, sizeof buf, hostAddr, hostLen);
		int count = 1;
		for (const Peer& p : peers)
			count += p.active;
		char s[128];
		snprintf(s, sizeof s, "Joined %s: %d player%s", address.c_str(), count, count == 1 ? "" : "s");
		status = s;
	}

	~Net()
	{
		quit = true;
		if (thread.joinable())
			thread.join();
		if (sock != kNoSocket)
			closeSocket(sock);
	}
};

Net& net()
{
	static Net n;
	static std::once_flag once;
	std::call_once(once, [] { n.start(); });
	return n;
}

} // namespace

extern "C" int port_net_active(void) { return net().mode != Net::OFF; }

extern "C" void port_net_publish(const NetPose* pose)
{
	Net& n = net();
	if (n.mode == Net::OFF)
		return;
	std::lock_guard<std::mutex> lk(n.mu);
	n.local = *pose;
	n.haveLocal = true;
}

extern "C" int port_net_remote(int slot, NetRemote* out)
{
	Net& n = net();
	memset(out, 0, sizeof *out);
	if (n.mode == Net::OFF || slot < 0 || slot >= NET_MAX_PLAYERS)
		return 0;
	std::lock_guard<std::mutex> lk(n.mu);
	if (slot == n.mySlot || !n.peers[slot].active || n.peers[slot].poseAt == 0)
		return 0;
	const Peer& p = n.peers[slot];
	out->connected = 1;
	memcpy(out->name, p.name, NET_NAME_LEN);
	out->pose = p.pose;
	out->age = float(now() - p.poseAt);
	return 1;
}

extern "C" int port_net_local_addresses(char* out, int size)
{
	if (size <= 0)
		return 0;
	out[0] = 0;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return 0;
#endif
	char host[256];
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	if (gethostname(host, sizeof host) == 0 && getaddrinfo(host, nullptr, &hints, &res) == 0) {
		std::string list;
		for (addrinfo* a = res; a; a = a->ai_next) {
			char ip[INET_ADDRSTRLEN] = {};
			const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(a->ai_addr);
			if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip) || !strncmp(ip, "127.", 4))
				continue;
			if (list.find(ip) == std::string::npos)
				list += (list.empty() ? "" : ", ") + std::string(ip);
		}
		freeaddrinfo(res);
		snprintf(out, size_t(size), "%s", list.c_str());
	}
#ifdef _WIN32
	WSACleanup();
#endif
	return int(strlen(out));
}

extern "C" const char* port_net_status(void)
{
	static std::string copy;
	Net& n = net();
	std::lock_guard<std::mutex> lk(n.mu);
	copy = n.status;
	return copy.c_str();
}
