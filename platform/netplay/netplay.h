// Online co-op (netplay): the network side, plain host code.
//
// One player hosts (UDP, SMS_NET_PORT, default 27016) and up to seven join.
// Every player sends the pose of their Mario about 60 times a second; the
// host relays all of them to everyone. The game side (net_game.cpp, built
// with the game) publishes the local pose and draws the others as puppets.
//
// Settings (settings.txt / the launcher's Online page):
//   SMS_NET_MODE     off, host or join
//   SMS_NET_ADDRESS  the host's address, for join
//   SMS_NET_PORT     UDP port (27016)
//   SMS_NET_NAME     the name shown to other players
#ifndef PORT_NETPLAY_H
#define PORT_NETPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NET_MAX_PLAYERS = 8, NET_NAME_LEN = 16 };

// Everything needed to draw one Mario as he looks on his player's screen:
// the body's base matrix after calcBaseMtx, both animation layers (body and
// upper body) with their frames and blend, the face texture pattern, and
// where he is (a remote Mario is drawn only in the same stage and episode).
typedef struct NetPose {
	float mtx[3][4];       // body base TR matrix
	uint16_t anm[2][2];    // [layer][new, old] BCK index
	float frame[3];        // frame controls 0 (body), 1 (upper), 2 (texture pattern)
	float blend[2];        // motion blend ratio of each layer
	uint8_t texPattern;    // 0xff: none
	uint8_t flags;         // NET_POSE_*
	uint8_t area, episode; // stage
	uint8_t hand;          // hand model set (TMario::changeHand)
	uint8_t nozzle;        // FLUDD's current nozzle; 0xff: no FLUDD
	uint8_t pad[2];
} NetPose;

enum { NET_POSE_VISIBLE = 1, NET_POSE_CAP = 2, NET_POSE_IN_STAGE = 4 };

typedef struct NetRemote {
	int connected;
	char name[NET_NAME_LEN];
	NetPose pose;
	float age; // seconds since this pose arrived
} NetRemote;

// 1 when netplay is on (hosting, or joined or joining).
int port_net_active(void);
// The local player's pose, every game frame.
void port_net_publish(const NetPose* pose);
// The other players, by slot (the local player's own slot reads as empty).
// Returns connected.
int port_net_remote(int slot, NetRemote* out);
// A short status line ("hosting on port 27016, 2 players", "joining ...").
const char* port_net_status(void);
// This computer's IPv4 addresses on its networks (not loopback), separated
// by ", ", for players to give to friends joining them. Returns the length.
int port_net_local_addresses(char* out, int size);

#ifdef __cplusplus
}
#endif
#endif
