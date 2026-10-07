// CARD: a memory card in slot A backed by host files; slot B is empty.
//
// Card directory: $SMS_SAVE_DIR, else $XDG_DATA_HOME/sms-port/card-a, else
// ~/.local/share/sms-port/card-a. Each card file is stored as <name>.dat
// (the file's bytes, exactly as the game wrote them, i.e. big-endian save
// data) plus <name>.stat (the CARDStat the game set). Emulates a 59-block
// card (4 Mbit, 8 KiB sectors). All operations complete synchronously.
//
// As on hardware, a file belongs to the game that made it: its CARDStat keeps
// the game code and maker of the running disc (DVDGetCurrentDiskID), and a
// game only sees its own files, by name or by number (CARD_RESULT_NOPERM, as
// the SDK's __CARDAccess). Super Mario Eclipse (GMSE04) therefore keeps
// saves apart from Super Mario Sunshine's (GMSE01) on the same card. Files of
// GMSE01 keep their plain names on disk; another game's are stored with its ID
// first (GMSE04_<name>). index.txt lists those on-disk names.
//
// Files are written to <file>.tmp and then moved into place, so a game that
// stops mid-write (a crash, or the window closed while saving) leaves the old
// file or the new one, never a cut-short one. A .dat shorter than its
// CARDStat length (left by such a stop before this) made every read past its
// end fail, which Sunshine reports as "The device in Slot A is not supported"
// on every start. On load such a file is copied to <file>.dat.damaged and
// padded back to its length, so the game's own checks see it: Sunshine keeps
// two copies of its data and uses an intact one, or offers a new save.
#include "port_compat.h"
#include "port_platform.h"
#include <dolphin/card.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "port_host.h"
#include <errno.h>
#include <dolphin/dvd.h>

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

// The running game's code and maker (GMSE / 01 for Sunshine), from the disc.
void current_game(void* game, void* company)
{
	memcpy(game, "GMSE", 4);
	memcpy(company, "01", 2);
	if (const DVDDiskID* id = DVDGetCurrentDiskID())
		if (id->gameName[0]) {
			memcpy(game, id->gameName, 4);
			memcpy(company, id->company, 2);
		}
}

bool is_current_game(const CARDStat& st)
{
	char game[4], company[2];
	current_game(game, company);
	return !memcmp(st.gameName, game, 4) && !memcmp(st.company, company, 2);
}

// The file's name on disk, without .dat/.stat: Sunshine's keep the plain name.
std::string disk_name(const CardFile& f)
{
	if (!memcmp(f.stat.gameName, "GMSE", 4) && !memcmp(f.stat.company, "01", 2))
		return safe(f.name);
	return safe(std::string((const char*)f.stat.gameName, 4) + std::string((const char*)f.stat.company, 2) + "_" + f.name);
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

// Writes path through path.tmp, so a stop mid-write never leaves a cut-short
// file. (Windows' rename does not replace: the old file goes first, and load
// picks up a .tmp left between the two steps.)
bool write_file(const std::string& path, const void* data, size_t size)
{
	const std::string tmp = path + ".tmp";
	FILE* fp              = fopen(tmp.c_str(), "wb");
	if (!fp)
		return false;
	const bool ok = fwrite(data, 1, size, fp) == size && fflush(fp) == 0;
	if (fclose(fp) != 0 || !ok) {
		remove(tmp.c_str());
		port_log("[card] could not write %s\n", path.c_str());
		return false;
	}
#ifdef _WIN32
	remove(path.c_str());
#endif
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		port_log("[card] could not replace %s\n", path.c_str());
		return false;
	}
	return true;
}

// a file whose write was cut between its two steps on Windows: the new one is complete
void finish_write(const std::string& path)
{
	FILE* fp = fopen(path.c_str(), "rb");
	if (fp) {
		fclose(fp);
		return;
	}
	rename((path + ".tmp").c_str(), path.c_str());
}

void save(int no)
{
	CardFile& f   = g_files[no];
	std::string b = card_dir() + "/" + disk_name(f);
	write_file(b + ".dat", f.data.data(), f.data.size());
	write_file(b + ".stat", &f.stat, sizeof f.stat);
}

void migrate_eclipse_settings();

void load()
{
	if (g_loaded)
		return;
	g_loaded = true;
	std::string dir = card_dir();
	finish_write(dir + "/index.txt");
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
		const std::string base = dir + "/" + safe(name);
		finish_write(base + ".dat");
		finish_write(base + ".stat");
		if (!read_all(base + ".dat", f.data) || !read_all(base + ".stat", st) || st.size() != sizeof(CARDStat))
			continue;
		memcpy(&f.stat, st.data(), sizeof f.stat);
		// the card file's own name, from its status (index.txt holds the name on disk)
		f.name = std::string(f.stat.fileName, strnlen(f.stat.fileName, CARD_FILENAME_MAX));
		if (f.name.empty())
			f.name = name;
		f.used = true;
		if (f.stat.length && f.data.size() != f.stat.length) {
			// cut short by a stop mid-write: keep a copy, then give the game whole sectors to check
			port_log("[card] %s is %u bytes, not %u: the game stopped while saving it. A copy is in "
			         "%s.dat.damaged; the game will check what is left\n",
			         name.c_str(), (unsigned)f.data.size(), (unsigned)f.stat.length, safe(name).c_str());
			write_file(base + ".dat.damaged", f.data.data(), f.data.size());
			f.data.resize(f.stat.length, 0);
			write_file(base + ".dat", f.data.data(), f.data.size());
		}
	}
	migrate_eclipse_settings();
}

void save_index()
{
	std::string t;
	for (int i = 0; i < kMaxFiles; i++)
		t += (g_files[i].used ? disk_name(g_files[i]) : std::string()) + "\n";
	while (t.size() > 1 && t[t.size() - 1] == '\n' && t[t.size() - 2] == '\n')
		t.erase(t.size() - 1);
	write_file(card_dir() + "/index.txt", t.data(), t.size());
}

// Super Mario Eclipse's settings, which BetterSunshineEngine names after the
// module, were saved by earlier builds as "super_mario_sunshine" (its settings
// group had lost its module), the name of Eclipse's game save: the game then
// found a one-sector file there and could not save ("The device in Slot A is
// not supported"). The game's save is never one sector (0xE000 bytes), so such
// a file is the settings; it moves to "super_mario_eclipse", where they are now.
void migrate_eclipse_settings()
{
	const char* from = "super_mario_sunshine";
	const char* to   = "super_mario_eclipse";
	for (int i = 0; i < kMaxFiles; i++) {
		CardFile& f = g_files[i];
		if (!f.used || f.name != from || memcmp(f.stat.gameName, "GMSE", 4) || memcmp(f.stat.company, "04", 2)
		    || f.stat.length != (u32)kSectorSize)
			continue;
		for (int j = 0; j < kMaxFiles; j++)
			if (g_files[j].used && g_files[j].name == to && !memcmp(g_files[j].stat.gameName, "GMSE", 4)
			    && !memcmp(g_files[j].stat.company, "04", 2))
				return;
		const std::string old_base = card_dir() + "/" + disk_name(f);
		f.name = to;
		memset(f.stat.fileName, 0, sizeof f.stat.fileName);
		strncpy(f.stat.fileName, to, CARD_FILENAME_MAX);
		save(i);
		save_index();
		remove((old_base + ".dat").c_str());
		remove((old_base + ".stat").c_str());
		port_log("[card] moved Super Mario Eclipse's settings from %s to %s, so its game can save\n", from, to);
		return;
	}
}

// The running game's file of that name (another game's files are not its own).
int find(const char* name)
{
	for (int i = 0; i < kMaxFiles; i++)
		if (g_files[i].used && g_files[i].name == name && is_current_game(g_files[i].stat))
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
extern "C" int CARDProbe(long chan) { return chan == 0; }
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
extern "C" long CARDCheck(long chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
extern "C" long CARDFormat(long chan)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	for (int i = 0; i < kMaxFiles; i++) {
		if (g_files[i].used) {
			std::string b = card_dir() + "/" + disk_name(g_files[i]);
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
	if (!is_current_game(g_files[fileNo].stat))
		return CARD_RESULT_NOPERM;
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
extern "C" long CARDCreate(long chan, char* fileName, unsigned long size, CARDFileInfo* fi)
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
		current_game(f.stat.gameName, f.stat.company);
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
extern "C" long CARDRead(CARDFileInfo* fi, void* buf, s32 length, s32 offset)
{
	if (fi->fileNo < 0 || fi->fileNo >= kMaxFiles || !g_files[fi->fileNo].used)
		return CARD_RESULT_NOFILE;
	std::vector<u8>& d = g_files[fi->fileNo].data;
	if (offset < 0 || (u32)(offset + length) > d.size())
		return CARD_RESULT_LIMIT;
	memcpy(buf, d.data() + offset, length);
	return CARD_RESULT_READY;
}
extern "C" long CARDWrite(CARDFileInfo* fi, void* buf, long length, long offset)
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
	if (!is_current_game(g_files[fileNo].stat))
		return CARD_RESULT_NOPERM;
	*stat = g_files[fileNo].stat;
	return CARD_RESULT_READY;
}
extern "C" long CARDSetStatus(long chan, long fileNo, CARDStat* stat)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	if (!is_current_game(g_files[fileNo].stat))
		return CARD_RESULT_NOPERM;
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
extern "C" long CARDFastDelete(long chan, long fileNo)
{
	if (chan != 0)
		return CARD_RESULT_NOCARD;
	if (fileNo < 0 || fileNo >= kMaxFiles || !g_files[fileNo].used)
		return CARD_RESULT_NOFILE;
	if (!is_current_game(g_files[fileNo].stat))
		return CARD_RESULT_NOPERM;
	std::string b = card_dir() + "/" + disk_name(g_files[fileNo]);
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
