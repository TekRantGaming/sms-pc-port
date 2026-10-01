// CARD: a memory card in slot A backed by host files; slot B is empty.
//
// Card directory: $SMS_SAVE_DIR, else $XDG_DATA_HOME/sms-port/card-a, else
// ~/.local/share/sms-port/card-a. Each card file is stored as <name>.dat
// (the file's bytes, exactly as the game wrote them, i.e. big-endian save
// data) plus <name>.stat (the CARDStat the game set). Emulates a 59-block
// card (4 Mbit, 8 KiB sectors). All operations complete synchronously.
#include "port_compat.h"
#include "port_platform.h"
#include <dolphin/card.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "port_host.h"
#include <errno.h>

namespace {

const s32 kSectorSize = 8192;
const s32 kBlocks     = 59;
const int kMaxFiles   = CARD_MAX_FILE;

struct CardFile {
	bool used;
	std::string name;
	CARDStat stat;
	std::vector<u8> data;
};

std::string g_dir;
bool g_mounted;
CardFile g_files[kMaxFiles];
bool g_loaded;

void mkdirs(const std::string& p)
{
	for (size_t i = 1; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/')
			port_mkdir(p.substr(0, i).c_str(), 0755);
}

std::string card_dir()
{
	if (!g_dir.empty())
		return g_dir;
	if (const char* d = getenv("SMS_SAVE_DIR"))
		g_dir = d;
	else if (const char* x = getenv("XDG_DATA_HOME"))
		g_dir = std::string(x) + "/sms-port/card-a";
#ifdef _WIN32
	else if (const char* x = getenv("APPDATA"))
		g_dir = std::string(x) + "/sms-port/card-a";
#endif
	else
		g_dir = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/share/sms-port/card-a";
	mkdirs(g_dir);
	port_log("[card] slot A: %s\n", g_dir.c_str());
	return g_dir;
}

std::string safe(const std::string& n)
{
	std::string s;
	for (size_t i = 0; i < n.size(); i++)
		s += (isalnum((unsigned char)n[i]) || n[i] == '_' || n[i] == '-' || n[i] == '.') ? n[i] : '_';
	return s;
}

bool read_all(const std::string& path, std::vector<u8>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	u8 buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(f);
	return true;
}

void save(int no)
{
	CardFile& f     = g_files[no];
	std::string b   = card_dir() + "/" + safe(f.name);
	FILE* fp        = fopen((b + ".dat").c_str(), "wb");
	if (fp) {
		fwrite(f.data.data(), 1, f.data.size(), fp);
		fclose(fp);
	}
	fp = fopen((b + ".stat").c_str(), "wb");
	if (fp) {
		fwrite(&f.stat, 1, sizeof f.stat, fp);
		fclose(fp);
	}
}

void load()
{
	if (g_loaded)
		return;
	g_loaded = true;
	std::string dir = card_dir();
	std::vector<u8> idx;
	if (!read_all(dir + "/index.txt", idx))
		return;
	std::string text(idx.begin(), idx.end());
	size_t pos = 0;
	int no     = 0;
	while (pos < text.size() && no < kMaxFiles) {
		size_t e         = text.find('\n', pos);
		std::string name = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
		pos              = e == std::string::npos ? text.size() : e + 1;
		if (name.empty()) {
			no++;
			continue;
		}
		CardFile& f = g_files[no++];
		std::vector<u8> st;
		if (!read_all(dir + "/" + safe(name) + ".dat", f.data) || !read_all(dir + "/" + safe(name) + ".stat", st)
		    || st.size() != sizeof(CARDStat))
			continue;
		memcpy(&f.stat, st.data(), sizeof f.stat);
		f.name = name;
		f.used = true;
	}
}

void save_index()
{
	std::string t;
	for (int i = 0; i < kMaxFiles; i++)
		t += (g_files[i].used ? g_files[i].name : std::string()) + "\n";
	while (t.size() > 1 && t[t.size() - 1] == '\n' && t[t.size() - 2] == '\n')
		t.erase(t.size() - 1);
	FILE* fp = fopen((card_dir() + "/index.txt").c_str(), "wb");
	if (fp) {
		fwrite(t.data(), 1, t.size(), fp);
		fclose(fp);
	}
}

int find(const char* name)
{
	for (int i = 0; i < kMaxFiles; i++)
		if (g_files[i].used && g_files[i].name == name)
			return i;
	return -1;
}

s32 used_blocks()
{
	s32 n = 0;
	for (int i = 0; i < kMaxFiles; i++)
		if (g_files[i].used)
			n += (s32)((g_files[i].data.size() + kSectorSize - 1) / kSectorSize);
	return n;
}

} // namespace

extern "C" void CARDInit(void) {}
extern "C" s32 CARDGetResultCode(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
extern "C" int CARDProbe(s32 chan) { return chan == 0; }
extern "C" s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (memSize)
		*memSize = 4; // Mbit: a "Memory Card 59"
	if (sectorSize)
		*sectorSize = kSectorSize;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDMount(s32 chan, void*, CARDCallback)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	load();
	g_mounted = true;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDMountAsync(s32 chan, void* work, CARDCallback detach, CARDCallback attach)
{
	s32 r = CARDMount(chan, work, detach);
	if (attach)
		attach(chan, r);
	return r;
}
extern "C" s32 CARDUnmount(s32 chan)
{
	g_mounted = false;
	return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}
extern "C" s32 CARDCheck(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
extern "C" s32 CARDFormat(s32 chan)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	for (int i = 0; i < kMaxFiles; i++) {
		if (g_files[i].used) {
			std::string b = card_dir() + "/" + safe(g_files[i].name);
			remove((b + ".dat").c_str());
			remove((b + ".stat").c_str());
		}
		g_files[i] = CardFile();
	}
	save_index();
	return CARD_RESULT_READY;
}
extern "C" s32 CARDFreeBlocks(s32 chan, s32* bytesFree, s32* filesFree)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	load();
	int nf = 0;
	for (int i = 0; i < kMaxFiles; i++)
		nf += !g_files[i].used;
	if (bytesFree)
		*bytesFree = (kBlocks - used_blocks()) * kSectorSize;
	if (filesFree)
		*filesFree = nf;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDGetSectorSize(s32 chan, u32* size)
{
	*size = kSectorSize;
	return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

extern "C" s32 CARDOpen(s32 chan, char* fileName, CARDFileInfo* fi)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	load();
	int no = find(fileName);
	if (no < 0)
		return CARD_RESULT_NOFILE;
	fi->chan   = chan;
	fi->fileNo = no;
	fi->offset = 0;
	fi->length = (s32)g_files[no].data.size();
	fi->iBlock = 0;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fi)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	fi->chan   = chan;
	fi->fileNo = fileNo;
	fi->offset = 0;
	fi->length = (s32)g_files[fileNo].data.size();
	return CARD_RESULT_READY;
}
extern "C" s32 CARDClose(CARDFileInfo* fi)
{
	fi->chan = -1;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDCreate(s32 chan, char* fileName, u32 size, CARDFileInfo* fi)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	load();
	if (strlen(fileName) > CARD_FILENAME_MAX)
		return CARD_RESULT_NAMETOOLONG;
	if (find(fileName) >= 0)
		return CARD_RESULT_EXIST;
	s32 blocks = (s32)((size + kSectorSize - 1) / kSectorSize);
	if (used_blocks() + blocks > kBlocks)
		return CARD_RESULT_INSSPACE;
	for (int i = 0; i < kMaxFiles; i++) {
		if (g_files[i].used)
			continue;
		CardFile& f = g_files[i];
		f           = CardFile();
		f.used      = true;
		f.name      = fileName;
		f.data.assign(size, 0);
		strncpy(f.stat.fileName, fileName, CARD_FILENAME_MAX);
		f.stat.length = (u32)size;
		f.stat.time   = (u32)(OSGetTime() / (OSTime)(__OSBusClock / 4));
		memcpy(f.stat.gameName, "GMSE", 4);
		memcpy(f.stat.company, "01", 2);
		f.stat.iconAddr    = 0xFFFFFFFF;
		f.stat.commentAddr = 0xFFFFFFFF;
		save(i);
		save_index();
		fi->chan   = chan;
		fi->fileNo = i;
		fi->offset = 0;
		fi->length = (s32)size;
		return CARD_RESULT_READY;
	}
	return CARD_RESULT_NOENT;
}
extern "C" s32 CARDRead(CARDFileInfo* fi, void* buf, s32 length, s32 offset)
{
	if (fi->fileNo < 0 || fi->fileNo >= kMaxFiles || !g_files[fi->fileNo].used)
		return CARD_RESULT_NOFILE;
	std::vector<u8>& d = g_files[fi->fileNo].data;
	if (offset < 0 || (u32)(offset + length) > d.size())
		return CARD_RESULT_LIMIT;
	memcpy(buf, d.data() + offset, length);
	return CARD_RESULT_READY;
}
extern "C" s32 CARDWrite(CARDFileInfo* fi, void* buf, s32 length, s32 offset)
{
	if (fi->fileNo < 0 || fi->fileNo >= kMaxFiles || !g_files[fi->fileNo].used)
		return CARD_RESULT_NOFILE;
	std::vector<u8>& d = g_files[fi->fileNo].data;
	if (offset < 0 || (u32)(offset + length) > d.size())
		return CARD_RESULT_LIMIT;
	memcpy(d.data() + offset, buf, length);
	g_files[fi->fileNo].stat.time = (u32)(OSGetTime() / (OSTime)(__OSBusClock / 4));
	save(fi->fileNo);
	return CARD_RESULT_READY;
}
extern "C" s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	*stat = g_files[fileNo].stat;
	return CARD_RESULT_READY;
}
extern "C" s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	CardFile& f = g_files[fileNo];
	// Only the icon/banner/comment fields are settable, as on hardware.
	f.stat.bannerFormat = stat->bannerFormat;
	f.stat.iconAddr     = stat->iconAddr;
	f.stat.iconFormat   = stat->iconFormat;
	f.stat.iconSpeed    = stat->iconSpeed;
	f.stat.commentAddr  = stat->commentAddr;
	save((int)fileNo);
	return CARD_RESULT_READY;
}
extern "C" s32 CARDFastDelete(s32 chan, s32 fileNo)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	std::string b = card_dir() + "/" + safe(g_files[fileNo].name);
	remove((b + ".dat").c_str());
	remove((b + ".stat").c_str());
	g_files[fileNo] = CardFile();
	save_index();
	return CARD_RESULT_READY;
}
extern "C" s32 CARDDelete(s32 chan, char* fileName)
{
	load();
	int no = find(fileName);
	return no < 0 ? CARD_RESULT_NOFILE : (s32)CARDFastDelete(chan, no);
}
