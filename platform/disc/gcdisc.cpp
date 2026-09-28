// GameCube disc-image reader: plain .iso/.gcm and CISO. See gcdisc.h.
//
// Disc layout (all big-endian):
//   0x000  boot.bin: game ID [6], ..., magic 0xC2339F3D at 0x1C, title at
//          0x20, DOL offset 0x420, FST offset 0x424, FST size 0x428,
//          max FST size 0x42C
//   0x440  bi2.bin (0x2000 bytes)
//   0x2440 apploader: header 0x20 bytes, code size at +0x14, trailer at +0x18
//   FST:   12-byte entries {u8 is_dir, u24 name offset, u32 offset|parent,
//          u32 size|next}; entry 0 = root, its `next` = entry count; the
//          string table follows the entries.
// CISO (Dolphin): "CISO", u32 block size (little-endian), u8 map[0x8000-8]
// (1 = block stored), stored blocks follow the 0x8000-byte header in order.
#include "gcdisc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif
#include "port_host.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#endif

#include <string>
#include <vector>

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

const uint32_t kMagic       = 0xC2339F3D;
const uint32_t kCisoHeader  = 0x8000;
const uint64_t kGCDiscSize  = 1459978240; // full-size GameCube disc

} // namespace

struct GCDisc {
	int fd;
	uint64_t base;      // offset of the image in the file (embedded: after the executable)
	uint64_t file_size; // bytes of the image in the file
	uint64_t size;
	// CISO
	bool ciso;
	uint32_t block_size;
	std::vector<int64_t> block_pos; // file offset of each block, -1 if not stored
	// disc
	uint8_t header[0x440];
	char id[7];
	std::vector<uint8_t> fst;
	std::vector<GCDiscEntry> entries;
	std::vector<std::string> names;
};

static uint64_t file_length(int fd)
{
#ifdef _WIN32
	__int64 n = _lseeki64(fd, 0, SEEK_END);
#elif defined(__linux__)
	off64_t n = lseek64(fd, 0, SEEK_END);
#else
	off_t n = lseek(fd, 0, SEEK_END);
#endif
	return n < 0 ? 0 : (uint64_t)n;
}

static bool pread_all(int fd, void* buf, size_t n, uint64_t off)
{
	uint8_t* b = (uint8_t*)buf;
	while (n) {
		ssize_t r = port_pread(fd, b, n, off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		b += r;
		n -= (size_t)r;
		off += (uint64_t)r;
	}
	return true;
}

extern "C" uint32_t gcdisc_read(GCDisc* d, uint64_t offset, void* buf, uint32_t size)
{
	if (!d || offset >= d->size)
		return 0;
	if (size > d->size - offset)
		size = (uint32_t)(d->size - offset);
	if (!d->ciso)
		return pread_all(d->fd, buf, size, d->base + offset) ? size : 0;
	uint8_t* out = (uint8_t*)buf;
	uint32_t done = 0;
	while (done < size) {
		uint64_t pos  = offset + done;
		uint64_t blk  = pos / d->block_size;
		uint32_t in   = (uint32_t)(pos % d->block_size);
		uint32_t n    = d->block_size - in;
		if (n > size - done)
			n = size - done;
		int64_t fpos = blk < d->block_pos.size() ? d->block_pos[blk] : -1;
		if (fpos < 0)
			memset(out + done, 0, n); // unstored blocks are zero
		else if (!pread_all(d->fd, out + done, n, d->base + (uint64_t)fpos + in))
			return done;
		done += n;
	}
	return done;
}

static bool setup_ciso(GCDisc* d)
{
	uint8_t hdr[kCisoHeader];
	if (!pread_all(d->fd, hdr, sizeof hdr, d->base) || memcmp(hdr, "CISO", 4) != 0)
		return false;
	d->ciso       = true;
	d->block_size = le32(hdr + 4);
	if (d->block_size == 0 || d->block_size > (64u << 20))
		return false;
	uint32_t nmap = kCisoHeader - 8;
	int64_t pos   = kCisoHeader;
	uint32_t last = 0;
	d->block_pos.assign(nmap, -1);
	for (uint32_t i = 0; i < nmap; i++)
		if (hdr[8 + i]) {
			d->block_pos[i] = pos;
			pos += d->block_size;
			last = i + 1;
		}
	d->block_pos.resize(last);
	d->size = (uint64_t)last * d->block_size;
	if (d->size < kGCDiscSize && (uint64_t)nmap * d->block_size >= kGCDiscSize)
		d->size = kGCDiscSize; // trailing unstored blocks read as zero
	return true;
}

static int open_ro(const char* path)
{
	return open(path, O_RDONLY
#ifdef __linux__
	            | O_CLOEXEC | O_LARGEFILE
#elif !defined(_WIN32)
	            | O_CLOEXEC
#else
	            | O_BINARY
#endif
	);
}

// Opens the image stored in `fd` at [base, base + size); takes `fd`.
static GCDisc* open_at(int fd, uint64_t base, uint64_t size, const char* path, int verbose)
{
	GCDisc* d    = new GCDisc();
	d->fd        = fd;
	d->base      = base;
	d->ciso      = false;
	d->file_size = size;
	d->size      = d->file_size;
	uint8_t magic[4] = { 0 };
	pread_all(fd, magic, 4, base);
	const char* why = NULL;
	if (memcmp(magic, "CISO", 4) == 0 && !setup_ciso(d))
		why = "bad CISO header";
	else if (!memcmp(magic, "RVZ\x01", 4) || !memcmp(magic, "WIA\x01", 4) || be32(magic) == 0xB10BC001)
		why = "RVZ/WIA/GCZ images are not supported; convert to .iso with Dolphin (Convert File...)";
	if (!why && gcdisc_read(d, 0, d->header, sizeof d->header) != sizeof d->header)
		why = "image too small";
	if (!why && be32(d->header + 0x1C) != kMagic)
		why = "not a GameCube disc (no 0xC2339F3D at 0x1C)";
	uint32_t fst_off = 0, fst_size = 0;
	if (!why) {
		memcpy(d->id, d->header, 6);
		d->id[6] = 0;
		fst_off  = be32(d->header + 0x424);
		fst_size = be32(d->header + 0x428);
		if (fst_size < 12 || fst_off >= d->size || fst_size > d->size - fst_off || fst_size > (64u << 20))
			why = "FST out of range";
	}
	if (!why) {
		d->fst.resize(fst_size);
		if (gcdisc_read(d, fst_off, d->fst.data(), fst_size) != fst_size)
			why = "cannot read FST";
	}
	if (!why) {
		uint32_t count = be32(&d->fst[8]);
		if (count == 0 || (uint64_t)count * 12 > fst_size)
			why = "bad FST entry count";
		else {
			const char* strings = (const char*)&d->fst[count * 12];
			uint32_t strsize    = fst_size - count * 12;
			d->entries.resize(count);
			d->names.resize(count);
			for (uint32_t i = 0; i < count && !why; i++) {
				const uint8_t* e = &d->fst[i * 12];
				GCDiscEntry& o   = d->entries[i];
				uint32_t noff    = be32(e) & 0xFFFFFF;
				if (i && noff >= strsize) {
					why = "FST name out of range";
					break;
				}
				d->names[i] = i ? std::string(strings + noff, strnlen(strings + noff, strsize - noff)) : std::string();
				o.is_dir    = e[0] != 0;
				o.parent    = o.is_dir ? be32(e + 4) : 0;
				o.next      = o.is_dir ? be32(e + 8) : 0;
				o.offset    = o.is_dir ? 0 : be32(e + 4);
				o.size      = o.is_dir ? 0 : be32(e + 8);
			}
			for (uint32_t i = 0; i < count; i++)
				d->entries[i].name = d->names[i].c_str();
		}
	}
	if (why) {
		if (verbose)
			fprintf(stderr, "[disc] %s: %s\n", path, why);
		gcdisc_close(d);
		return NULL;
	}
	return d;
}

extern "C" GCDisc* gcdisc_open(const char* path, int verbose)
{
	int fd = open_ro(path);
	if (fd < 0) {
		if (verbose)
			fprintf(stderr, "[disc] cannot open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	return open_at(fd, 0, file_length(fd), path, verbose);
}

// Embedded image: tools/bundle_disc.py appends the image to the executable,
// then a 32-byte trailer {"SMSDISC1", u64 LE image offset, u64 LE image
// size, 8 reserved bytes} as the last bytes of the file.
extern "C" int gcdisc_self_path(char* buf, uint32_t bufsize)
{
	if (!bufsize)
		return 0;
#ifdef _WIN32
	DWORD n = GetModuleFileNameA(NULL, buf, bufsize);
	if (n == 0 || n >= bufsize)
		return 0;
#elif defined(__APPLE__)
	char raw[4096];
	uint32_t rawsize = sizeof raw;
	if (_NSGetExecutablePath(raw, &rawsize) != 0)
		return 0;
	char resolved[PATH_MAX];
	if (!realpath(raw, resolved) || strlen(resolved) >= bufsize)
		return 0;
	strcpy(buf, resolved);
#else
	// A private 32-bit loader allows an x64 OS without system multilib. In that
	// case /proc/self/exe names the loader; run.sh records the actual game.
	const char* launched = getenv("SMS_GAME_EXECUTABLE");
	if (launched && *launched) {
		if (strlen(launched) >= bufsize)
			return 0;
		strcpy(buf, launched);
		return 1;
	}
	ssize_t n = readlink("/proc/self/exe", buf, bufsize - 1);
	if (n <= 0)
		return 0;
	buf[n] = 0;
#endif
	return 1;
}

#ifdef __APPLE__
// SMS.app (tools/make_mac_app.sh) keeps the image beside the executable in
// Contents/Resources: data appended to the executable would break its code
// signature.
static GCDisc* open_app_resource(const char* self, int verbose)
{
	std::string path(self);
	size_t slash = path.rfind('/');
	if (slash == std::string::npos)
		return NULL;
	path.resize(slash);
	path += "/../Resources/disc.gcm";
	if (access(path.c_str(), R_OK) != 0)
		return NULL;
	return gcdisc_open(path.c_str(), verbose);
}
#endif

extern "C" GCDisc* gcdisc_open_embedded(int verbose)
{
	char self[4096];
	if (!gcdisc_self_path(self, sizeof self))
		return NULL;
	int fd = open_ro(self);
	if (fd < 0)
		return NULL;
	uint64_t len = file_length(fd);
	uint8_t t[32];
	if (len < sizeof t || !pread_all(fd, t, sizeof t, len - sizeof t) || memcmp(t, "SMSDISC1", 8) != 0) {
		close(fd);
#ifdef __APPLE__
		return open_app_resource(self, verbose);
#else
		return NULL;
#endif
	}
	uint64_t off = 0, size = 0;
	for (int i = 7; i >= 0; i--) {
		off  = off << 8 | t[8 + i];
		size = size << 8 | t[16 + i];
	}
	if (off > len - sizeof t || size > len - sizeof t - off) {
		if (verbose)
			fprintf(stderr, "[disc] %s: embedded disc trailer out of range\n", self);
		close(fd);
		return NULL;
	}
	return open_at(fd, off, size, self, verbose);
}

extern "C" void gcdisc_close(GCDisc* d)
{
	if (!d)
		return;
	if (d->fd >= 0)
		close(d->fd);
	delete d;
}

extern "C" int gcdisc_probe(const char* path)
{
	GCDisc* d = gcdisc_open(path, 0);
	gcdisc_close(d);
	return d != NULL;
}

extern "C" const char* gcdisc_game_id(const GCDisc* d) { return d->id; }
extern "C" const uint8_t* gcdisc_header(const GCDisc* d) { return d->header; }
extern "C" uint64_t gcdisc_size(const GCDisc* d) { return d->size; }

extern "C" int gcdisc_system_file(const GCDisc* d, int which, uint32_t* offset, uint32_t* size)
{
	uint32_t off = 0, sz = 0;
	switch (which) {
	case GCDISC_BOOT:
		off = 0, sz = 0x440;
		break;
	case GCDISC_BI2:
		off = 0x440, sz = 0x2000;
		break;
	case GCDISC_APPLOADER: {
		uint8_t h[0x20];
		if (gcdisc_read((GCDisc*)d, 0x2440, h, sizeof h) != sizeof h)
			return 0;
		off = 0x2440, sz = 0x20 + be32(h + 0x14) + be32(h + 0x18);
		break;
	}
	case GCDISC_DOL: {
		// DOL header: 7 text + 11 data {offset, address, size}; size = max end.
		off = be32(d->header + 0x420);
		uint8_t h[0x100];
		if (!off || gcdisc_read((GCDisc*)d, off, h, sizeof h) != sizeof h)
			return 0;
		for (int i = 0; i < 18; i++) {
			uint32_t o = be32(h + i * 4), s = be32(h + 0x90 + i * 4);
			if (s && o + s > sz)
				sz = o + s;
		}
		break;
	}
	case GCDISC_FST:
		off = be32(d->header + 0x424), sz = be32(d->header + 0x428);
		break;
	default:
		return 0;
	}
	if (offset)
		*offset = off;
	if (size)
		*size = sz;
	return sz != 0;
}

extern "C" uint32_t gcdisc_entry_count(const GCDisc* d) { return (uint32_t)d->entries.size(); }

extern "C" int gcdisc_entry(const GCDisc* d, uint32_t index, GCDiscEntry* out)
{
	if (index >= d->entries.size())
		return 0;
	*out = d->entries[index];
	return 1;
}

extern "C" const uint8_t* gcdisc_fst(const GCDisc* d, uint32_t* size)
{
	if (size)
		*size = (uint32_t)d->fst.size();
	return d->fst.data();
}

extern "C" int32_t gcdisc_lookup(const GCDisc* d, const char* path)
{
	uint32_t dir = 0;
	while (*path == '/')
		path++;
	while (*path) {
		const char* end = strchr(path, '/');
		size_t len      = end ? (size_t)(end - path) : strlen(path);
		if (len == 0 || (len == 1 && path[0] == '.')) {
			// "//" or "./": stay
		} else if (len == 2 && path[0] == '.' && path[1] == '.') {
			dir = d->entries[dir].parent;
		} else {
			uint32_t i = dir + 1, found = 0;
			while (i < d->entries[dir].next && i < d->entries.size()) {
				const GCDiscEntry& e = d->entries[i];
				if (strlen(e.name) == len && strncasecmp(e.name, path, len) == 0) {
					found = i;
					break;
				}
				i = e.is_dir ? e.next : i + 1;
			}
			if (!found)
				return -1;
			if (end && !d->entries[found].is_dir)
				return -1;
			dir = found;
		}
		if (!end)
			break;
		path = end + 1;
	}
	return (int32_t)dir;
}

extern "C" uint32_t gcdisc_path(const GCDisc* d, uint32_t index, char* buf, uint32_t bufsize)
{
	std::string p;
	if (index >= d->entries.size())
		return 0;
	// Walk up: a file's directory is the nearest preceding dir whose range covers it.
	std::vector<uint32_t> chain;
	uint32_t i = index;
	while (i != 0) {
		chain.push_back(i);
		uint32_t parent = 0;
		if (d->entries[i].is_dir)
			parent = d->entries[i].parent;
		else
			for (uint32_t j = i; j-- > 0;)
				if (d->entries[j].is_dir && d->entries[j].next > i) {
					parent = j;
					break;
				}
		i = parent;
	}
	for (size_t k = chain.size(); k-- > 0;)
		p += "/" + d->names[chain[k]];
	if (p.empty())
		p = "/";
	if (bufsize) {
		size_t n = p.size() < bufsize - 1 ? p.size() : bufsize - 1;
		memcpy(buf, p.data(), n);
		buf[n] = 0;
	}
	return (uint32_t)p.size();
}

extern "C" uint32_t gcdisc_read_file(GCDisc* d, uint32_t index, uint32_t file_offset, void* buf, uint32_t size)
{
	if (index >= d->entries.size() || d->entries[index].is_dir)
		return 0;
	const GCDiscEntry& e = d->entries[index];
	if (file_offset >= e.size)
		return 0;
	if (size > e.size - file_offset)
		size = e.size - file_offset;
	return gcdisc_read(d, (uint64_t)e.offset + file_offset, buf, size);
}
