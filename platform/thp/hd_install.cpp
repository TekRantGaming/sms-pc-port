// HD cutscene installation for the launcher, in C++ so release builds need
// no Python: the catalog (tools/media/cutscene-release.json, embedded at
// build time), SHA-256, and applying one SMP1 movie patch to the original
// movie read from the player's disc. tools/media/movie_patch.py is the
// reference; the rebuilt movie must match the catalog's target checksum.
//
// An SMP1 patch is "SMP1", a big-endian length and a JSON identity header,
// then each video frame of the HD movie as a length-prefixed JPEG. The HD
// movie is the original THP with those frames in place of its own: the
// same audio packets, timing and header, with the new frame sizes.
#include "hd_install.h"
#include "disc/gcdisc.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

const char kCatalog[] =
#include "hd_catalog.inc"
    ;

// --- SHA-256 (FIPS 180-4)
struct Sha256 {
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t block[64];
	size_t used  = 0;
	uint64_t bits = 0;

	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
	void compress(const uint8_t* p)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
	}
	void update(const void* data, size_t n)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		bits += uint64_t(n) * 8;
		while (n) {
			const size_t take = std::min(n, sizeof block - used);
			memcpy(block + used, p, take);
			used += take, p += take, n -= take;
			if (used == sizeof block) {
				compress(block);
				used = 0;
			}
		}
	}
	std::string hex()
	{
		const uint64_t total = bits;
		const uint8_t one    = 0x80, zero = 0;
		update(&one, 1);
		while (used != 56)
			update(&zero, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = uint8_t(total >> (56 - i * 8));
		update(len, 8);
		char out[65];
		for (int i = 0; i < 8; i++)
			snprintf(out + i * 8, 9, "%08x", h[i]);
		return out;
	}
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put32(uint8_t* p, uint32_t v)
{
	p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// The value of "key" in the JSON object text [begin, end): a string, or the
// digits of a number.
std::string field(const char* begin, const char* end, const char* key)
{
	const std::string needle = std::string("\"") + key + "\"";
	const char* p            = std::search(begin, end, needle.begin(), needle.end());
	if (p == end)
		return std::string();
	p += needle.size();
	while (p < end && (*p == ' ' || *p == ':'))
		p++;
	if (p < end && *p == '"') {
		const char* q = std::find(p + 1, end, '"');
		return std::string(p + 1, q);
	}
	const char* q = p;
	while (q < end && (*q == '-' || (*q >= '0' && *q <= '9')))
		q++;
	return std::string(p, q);
}

void fail(char* err, int size, const char* msg)
{
	if (err && size > 0)
		snprintf(err, size_t(size), "%s", msg);
}

FILE* openUtf8(const char* path, const char* mode)
{
#ifdef _WIN32
	wchar_t wpath[1024], wmode[8];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024))
		return nullptr;
	MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
	return _wfopen(wpath, wmode);
#else
	return fopen(path, mode);
#endif
}

} // namespace

extern "C" const char* port_hd_catalog_json(void) { return kCatalog; }

extern "C" int port_hd_catalog(PortHdMovie* out, int max, char* release, int releaseSize)
{
	const char* begin = kCatalog;
	const char* end   = kCatalog + strlen(kCatalog);
	if (release && releaseSize > 0)
		snprintf(release, size_t(releaseSize), "%s", field(begin, end, "release").c_str());
	const char* p = strstr(begin, "\"movies\"");
	int n         = 0;
	while (p && (p = strchr(p, '{')) != nullptr && n < max) {
		const char* q = strchr(p, '}');
		if (!q)
			break;
		PortHdMovie& m = out[n++];
		memset(&m, 0, sizeof m);
		snprintf(m.disc_path, sizeof m.disc_path, "%s", field(p, q, "disc_path").c_str());
		snprintf(m.patch_file, sizeof m.patch_file, "%s", field(p, q, "patch_file").c_str());
		snprintf(m.url, sizeof m.url, "%s", field(p, q, "url").c_str());
		snprintf(m.source_sha256, sizeof m.source_sha256, "%s", field(p, q, "source_sha256").c_str());
		snprintf(m.target_sha256, sizeof m.target_sha256, "%s", field(p, q, "target_sha256").c_str());
		snprintf(m.patch_sha256, sizeof m.patch_sha256, "%s", field(p, q, "patch_sha256").c_str());
		m.target_bytes = atoll(field(p, q, "target_bytes").c_str());
		m.patch_bytes  = atoll(field(p, q, "patch_bytes").c_str());
		m.width        = atoi(field(p, q, "width").c_str());
		m.height       = atoi(field(p, q, "height").c_str());
		p              = q + 1;
	}
	return n;
}

extern "C" int port_hd_sha256_file(const char* path, char hex[65])
{
	FILE* f = openUtf8(path, "rb");
	if (!f)
		return 0;
	Sha256 sha;
	std::vector<uint8_t> buf(1 << 20);
	size_t n;
	while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
		sha.update(buf.data(), n);
	fclose(f);
	snprintf(hex, 65, "%s", sha.hex().c_str());
	return 1;
}

extern "C" int port_hd_apply(const char* disc, const PortHdMovie* m, const char* patchPath, const char* outPath,
                             char* err, int errSize)
{
	// the original movie, from the player's disc image
	GCDisc* d = strcmp(disc, "bundled") == 0 ? gcdisc_open_embedded(0) : gcdisc_open(disc, 0);
	if (!d)
		return fail(err, errSize, "Cannot open the disc image"), 0;
	const int32_t index = gcdisc_lookup(d, m->disc_path);
	GCDiscEntry entry;
	if (index < 0 || !gcdisc_entry(d, uint32_t(index), &entry) || entry.is_dir) {
		gcdisc_close(d);
		return fail(err, errSize, "The disc image has no such movie"), 0;
	}
	std::vector<uint8_t> src(entry.size);
	const uint32_t got = gcdisc_read_file(d, uint32_t(index), 0, src.data(), entry.size);
	gcdisc_close(d);
	if (got != entry.size || src.size() < 0x30)
		return fail(err, errSize, "Cannot read the movie from the disc image"), 0;
	{
		Sha256 sha;
		sha.update(src.data(), src.size());
		if (sha.hex() != m->source_sha256)
			return fail(err, errSize, "Your disc's movie differs from the one the HD patch needs"), 0;
	}

	// its layout: one video component first, then audio
	const uint8_t* s = src.data();
	if (memcmp(s, "THP\0", 4) != 0 || (be32(s + 4) != 0x10000 && be32(s + 4) != 0x11000))
		return fail(err, errSize, "Not a THP movie"), 0;
	const bool v11      = be32(s + 4) == 0x11000;
	const uint32_t comp = be32(s + 0x20);
	if (comp + 20 > src.size())
		return fail(err, errSize, "Invalid THP header"), 0;
	const uint32_t count = be32(s + comp);
	if (count < 1 || count > 16 || s[comp + 4] != 0 || be32(s + 0x24) != 0)
		return fail(err, errSize, "Unsupported THP layout"), 0;
	std::vector<uint8_t> kinds(s + comp + 4, s + comp + 4 + count);
	uint32_t audioTracks = 1, info = comp + 20;
	for (uint32_t i = 0; i < count; i++) {
		if (kinds[i] == 0)
			info += v11 ? 12 : 8;
		else if (kinds[i] == 1) {
			if (v11)
				audioTracks = be32(s + info + 12);
			info += v11 ? 16 : 12;
		} else
			return fail(err, errSize, "Unsupported THP component"), 0;
	}
	const uint32_t frames = be32(s + 0x14);

	// the original frames: component sizes and their audio packets
	struct Frame {
		std::vector<uint32_t> sizes;
		uint32_t audioAt, audioLen;
	};
	std::vector<Frame> orig(frames);
	{
		uint64_t offset = be32(s + 0x28), size = be32(s + 0x18);
		for (uint32_t f = 0; f < frames; f++) {
			if (size < 8 + 4 * count || offset + size > src.size())
				return fail(err, errSize, "Invalid THP frame chain"), 0;
			Frame& fr = orig[f];
			fr.sizes.resize(count);
			for (uint32_t i = 0; i < count; i++)
				fr.sizes[i] = be32(s + offset + 8 + 4 * i);
			uint64_t cursor = offset + 8 + 4 * count + fr.sizes[0];
			fr.audioAt      = uint32_t(cursor);
			uint64_t audio  = 0;
			for (uint32_t i = 1; i < count; i++)
				audio += uint64_t(fr.sizes[i]) * (kinds[i] == 1 ? audioTracks : 1);
			if (cursor + audio > offset + size)
				return fail(err, errSize, "Invalid THP component size"), 0;
			fr.audioLen          = uint32_t(audio);
			const uint64_t next = be32(s + offset);
			offset += size;
			size = next;
		}
	}

	// the patch: header, then each frame's JPEG length and position
	FILE* pf = openUtf8(patchPath, "rb");
	if (!pf)
		return fail(err, errSize, "Cannot open the movie patch"), 0;
	uint8_t head[8];
	if (fread(head, 1, 8, pf) != 8 || memcmp(head, "SMP1", 4) != 0 || be32(head + 4) == 0 || be32(head + 4) > 65536) {
		fclose(pf);
		return fail(err, errSize, "Not an SMP1 movie patch"), 0;
	}
	fseek(pf, long(be32(head + 4)), SEEK_CUR);
	std::vector<uint32_t> jpegLen(frames), sizes(frames);
	std::vector<long long> jpegAt(frames);
	for (uint32_t f = 0; f < frames; f++) {
		uint8_t lb[4];
		if (fread(lb, 1, 4, pf) != 4) {
			fclose(pf);
			return fail(err, errSize, "The movie patch is truncated"), 0;
		}
		jpegLen[f] = be32(lb);
		if (jpegLen[f] < 4 || jpegLen[f] > (32u << 20)) {
			fclose(pf);
			return fail(err, errSize, "Invalid frame in the movie patch"), 0;
		}
#ifdef _WIN32
		jpegAt[f] = _ftelli64(pf);
		_fseeki64(pf, jpegLen[f], SEEK_CUR);
#else
		jpegAt[f] = ftello(pf);
		fseeko(pf, jpegLen[f], SEEK_CUR);
#endif
		sizes[f] = (8 + 4 * count + jpegLen[f] + orig[f].audioLen + 31) & ~31u;
	}

	// the new header: dimensions and frame sizes
	std::vector<uint8_t> prefix(s, s + be32(s + 0x28));
	uint64_t total = 0;
	uint32_t largest = 0;
	for (uint32_t f = 0; f < frames; f++)
		total += sizes[f], largest = std::max(largest, sizes[f]);
	put32(&prefix[comp + 20], uint32_t(m->width));
	put32(&prefix[comp + 24], uint32_t(m->height));
	put32(&prefix[8], largest);
	put32(&prefix[0x18], sizes[0]);
	put32(&prefix[0x1c], uint32_t(total));
	put32(&prefix[0x2c], uint32_t(prefix.size() + total - sizes[frames - 1]));
	if (prefix.size() + total != uint64_t(m->target_bytes)) {
		fclose(pf);
		return fail(err, errSize, "The rebuilt movie's size differs from the catalog"), 0;
	}

	// write it, hashing as it goes
	const std::string tmp = std::string(outPath) + ".part";
	FILE* out             = openUtf8(tmp.c_str(), "wb");
	if (!out) {
		fclose(pf);
		return fail(err, errSize, "Cannot write the HD movie"), 0;
	}
	Sha256 sha;
	bool ok = true;
	auto emit = [&](const void* p, size_t n) {
		sha.update(p, n);
		ok = ok && fwrite(p, 1, n, out) == n;
	};
	emit(prefix.data(), prefix.size());
	std::vector<uint8_t> frame;
	for (uint32_t f = 0; f < frames && ok; f++) {
		frame.assign(sizes[f], 0);
		put32(&frame[0], sizes[(f + 1) % frames]);
		put32(&frame[4], sizes[(f + frames - 1) % frames]);
		put32(&frame[8], jpegLen[f]);
		for (uint32_t i = 1; i < count; i++)
			put32(&frame[8 + 4 * i], orig[f].sizes[i]);
		uint8_t* jpeg = &frame[8 + 4 * count];
#ifdef _WIN32
		_fseeki64(pf, jpegAt[f], SEEK_SET);
#else
		fseeko(pf, jpegAt[f], SEEK_SET);
#endif
		if (fread(jpeg, 1, jpegLen[f], pf) != jpegLen[f] || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
			ok = false;
			break;
		}
		memcpy(jpeg + jpegLen[f], s + orig[f].audioAt, orig[f].audioLen);
		emit(frame.data(), frame.size());
	}
	fclose(pf);
	ok = (fclose(out) == 0) && ok;
	if (!ok || sha.hex() != m->target_sha256) {
		remove(tmp.c_str());
		return fail(err, errSize, ok ? "The HD movie failed its final checksum" : "Writing the HD movie failed (is the disk full?)"), 0;
	}
	remove(outPath);
	if (rename(tmp.c_str(), outPath) != 0) {
		remove(tmp.c_str());
		return fail(err, errSize, "Cannot move the HD movie into place"), 0;
	}
	return 1;
}
