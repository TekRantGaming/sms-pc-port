// Offline test of the software DSP mixer against the real disc data.
//
//   audio_test DISC_FILES_DIR [OUT_DIR]
//
// Reads mSound.aaf out of data/nintendo.szs (Yaz0 + RARC), walks its WSYS
// wave systems, loads the referenced .aw wave archives into a fake ARAM and
// renders selected waves through the same voice blocks JAudio writes
// (DSPBuffer layout, see ../PROTOCOL.md) with the mixer's 80-sample
// subframes, writing 32 kHz stereo WAV files and printing length and level.
// (.cc so the port's GLOB_RECURSE over platform/*.cpp does not pick it up.)
#include "../dsp_mixer.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

typedef std::vector<uint8_t> Bytes;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool read_file(const std::string& path, Bytes& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	out.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	size_t n = fread(out.data(), 1, out.size(), f);
	fclose(f);
	return n == out.size();
}

static Bytes yaz0(const Bytes& in)
{
	if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4) != 0)
		return in;
	uint32_t n = be32(&in[4]);
	Bytes o;
	o.reserve(n);
	size_t p = 16;
	while (o.size() < n) {
		uint8_t c = in[p++];
		for (int b = 0; b < 8 && o.size() < n; b++) {
			if (c & (0x80 >> b)) {
				o.push_back(in[p++]);
			} else {
				uint32_t x = be16(&in[p]);
				p += 2;
				uint32_t dist = (x & 0xFFF) + 1, len = x >> 12;
				len = len ? len + 2 : in[p++] + 0x12;
				for (uint32_t i = 0; i < len; i++)
					o.push_back(o[o.size() - dist]);
			}
		}
	}
	return o;
}

// Find a file by (case-insensitive) name in a RARC archive.
static bool rarc_find(const Bytes& d, const char* want, Bytes& out)
{
	if (d.size() < 0x40 || memcmp(d.data(), "RARC", 4) != 0)
		return false;
	uint32_t data = be32(&d[0xC]) + 0x20;
	const uint8_t* info = &d[0x20];
	uint32_t nfiles = be32(info + 8), foff = be32(info + 0xC) + 0x20, soff = be32(info + 0x14) + 0x20;
	for (uint32_t i = 0; i < nfiles; i++) {
		const uint8_t* e = &d[foff + i * 20];
		uint16_t type = be16(e + 4), name = be16(e + 6);
		if (!(type & 0x100))
			continue;
		const char* s = (const char*)&d[soff + name];
		if (strcasecmp(s, want) == 0) {
			uint32_t o = be32(e + 8), sz = be32(e + 12);
			out.assign(d.begin() + data + o, d.begin() + data + o + sz);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------

struct Wave {
	std::string arc;
	int index, format, key;
	float rate;
	uint32_t offset, size, loop, loopStart, loopEnd, count;
	int16_t yn1, yn2;
};

static Bytes g_aram(16u << 20);
static const uint8_t* aram_at(uint32_t a) { return a < g_aram.size() ? &g_aram[a] : NULL; }

// Voice blocks as the game allocates them (64 x 0x180, 32-byte aligned).
alignas(32) static uint8_t g_voices[64][0x180];
static std::vector<int16_t> fxMemory[4];
static int16_t* resolve_fx(uint32_t addr, uint32_t samples)
{
	if (addr < 1 || addr > 4 || fxMemory[addr - 1].size() < samples)
		return NULL;
	return fxMemory[addr - 1].data();
}
alignas(32) static uint8_t g_fx[4][0x20];

template <class T> static void put(uint8_t* b, int off, T v) { memcpy(b + off, &v, sizeof v); }
template <class T> static T get(const uint8_t* b, int off)
{
	T v;
	memcpy(&v, b + off, sizeof v);
	return v;
}

// What DSPBuffer::setWaveInfo / playStart / setMixerInitVolume / setPitch /
// setBusConnect write, for a sampled wave (see JASDSPInterface.cpp).
static void setup_voice(uint8_t* v, const Wave& w, uint32_t base, double ratio, int16_t volL, int16_t volR, int16_t fx)
{
	static const uint8_t kSrc[4] = { 9, 5, 8, 16 };
	memset(v, 0, 0x180);
	put<uint32_t>(v, 0x118, base);
	put<uint16_t>(v, 0x100, kSrc[w.format & 3]);
	put<int32_t>(v, 0x11C, (int32_t)w.count);
	put<uint16_t>(v, 0x102, (uint16_t)(w.loop ? 1 : 0));
	if (w.loop) {
		put<uint32_t>(v, 0x110, w.loopStart);
		put<uint32_t>(v, 0x114, w.loopEnd);
		put<int16_t>(v, 0x104, w.yn1);
		put<int16_t>(v, 0x106, w.yn2);
	} else {
		put<uint32_t>(v, 0x114, w.count);
	}
	uint32_t r = (uint32_t)lrint(ratio * 4096.0);
	put<uint16_t>(v, 0x004, (uint16_t)(r > 0x7FFF ? 0x7FFF : r));
	const uint16_t bus[4] = { 0x0D00, 0x0D60, 0x0DC0, 0x0E20 };
	const int16_t vol[4] = { volL, volR, (int16_t)(volL * fx / 127), (int16_t)(volR * fx / 127) };
	for (int s = 0; s < 4; s++) {
		put<uint16_t>(v, 0x10 + s * 8 + 0, bus[s]);
		put<int16_t>(v, 0x10 + s * 8 + 2, vol[s]);
		put<int16_t>(v, 0x10 + s * 8 + 4, vol[s]);
	}
	put<uint16_t>(v, 0x008, 1); // resetVpb
	put<uint16_t>(v, 0x000, 1); // enabled
}

struct Render {
	std::vector<int16_t> pcm; // interleaved L,R
	int peak;
	double rms;
	int subframes;
};

// Renders subframes until voice 0 reports done (or `max_sec`). If
// `release_after` > 0 the voice is stopped like TDSPChannel::forceStop.
static Render render(double max_sec, double release_after, uint16_t master = 0x5000)
{
	Render r = {};
	int16_t a[80], b[80];
	int maxsub = (int)(max_sec * 32000 / 80);
	int rel = release_after > 0 ? (int)(release_after * 32000 / 80) : -1;
	int tail = 0;
	for (int s = 0; s < maxsub; s++) {
		if (s == rel)
			put<uint16_t>(g_voices[0], 0x10A, 1);
		port_dspmix_render(a, b, 80, master);
		for (int k = 0; k < 80; k++) {
			r.pcm.push_back(a[k]);
			r.pcm.push_back(b[k]);
		}
		r.subframes++;
		if (get<uint16_t>(g_voices[0], 0x002)) {
			// done: CPU's replyFinishRequest
			put<uint16_t>(g_voices[0], 0x002, 0);
			put<uint16_t>(g_voices[0], 0x000, 0);
		}
		if (!get<uint16_t>(g_voices[0], 0x000) && ++tail > 400 * 1) // keep FX tail ~1 s
			break;
	}
	double acc = 0;
	for (size_t i = 0; i < r.pcm.size(); i++) {
		int v = abs(r.pcm[i]);
		if (v > r.peak)
			r.peak = v;
		acc += (double)r.pcm[i] * r.pcm[i];
	}
	r.rms = r.pcm.empty() ? 0 : sqrt(acc / r.pcm.size());
	return r;
}

static void write_wav(const std::string& path, const std::vector<int16_t>& pcm)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	uint32_t data = (uint32_t)pcm.size() * 2, v32;
	uint16_t v16;
	fwrite("RIFF", 1, 4, f);
	v32 = 36 + data;
	fwrite(&v32, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	v32 = 16;
	fwrite(&v32, 4, 1, f);
	v16 = 1;
	fwrite(&v16, 2, 1, f);
	v16 = 2;
	fwrite(&v16, 2, 1, f);
	v32 = 32000;
	fwrite(&v32, 4, 1, f);
	v32 = 32000 * 4;
	fwrite(&v32, 4, 1, f);
	v16 = 4;
	fwrite(&v16, 2, 1, f);
	v16 = 16;
	fwrite(&v16, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	fwrite(pcm.data(), 2, pcm.size(), f); // host little-endian = WAV order
	fclose(f);
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s DISC_FILES_DIR [OUT_DIR]  (the disc's extracted files/ folder)\n", argv[0]);
		return 2;
	}
	std::string disc = argv[1];
	std::string out  = argc > 2 ? argv[2] : ".";
	Bytes szs, aaf;
	if (!read_file(disc + "/data/nintendo.szs", szs) || !rarc_find(yaz0(szs), "mSound.aaf", aaf)) {
		fprintf(stderr, "cannot read mSound.aaf from %s/data/nintendo.szs\n", disc.c_str());
		return 1;
	}
	printf("mSound.aaf: %zu bytes\n", aaf.size());

	// AAF command walk (JAIBasic::checkInitDataOnMemory): lists for 2 and 3.
	std::vector<Wave> waves;
	std::vector<uint32_t> fxScene; // cmd 7 blob offset
	size_t i = 0;
	for (;;) {
		uint32_t c = be32(&aaf[i * 4]);
		i++;
		if (c == 0)
			break;
		if (c == 2 || c == 3) {
			for (; be32(&aaf[i * 4]); i += 3) {
				if (c != 3)
					continue;
				uint32_t base = be32(&aaf[i * 4]);
				const uint8_t* ws = &aaf[base];
				uint32_t winf = base + be32(ws + 0x10);
				uint32_t narc = be32(&aaf[winf + 4]);
				for (uint32_t a = 0; a < narc; a++) {
					uint32_t arc = base + be32(&aaf[winf + 8 + a * 4]);
					std::string name((const char*)&aaf[arc]);
					uint32_t nw = be32(&aaf[arc + 0x70]);
					for (uint32_t k = 0; k < nw; k++) {
						const uint8_t* t = &aaf[base + be32(&aaf[arc + 0x74 + k * 4])];
						Wave w;
						w.arc    = name;
						w.index  = (int)k;
						w.format = t[1];
						w.key    = t[2];
						uint32_t fr = be32(t + 4);
						memcpy(&w.rate, &fr, 4);
						w.offset    = be32(t + 8);
						w.size      = be32(t + 0xC);
						w.loop      = be32(t + 0x10);
						w.loopStart = be32(t + 0x14);
						w.loopEnd   = be32(t + 0x18);
						w.count     = be32(t + 0x1C);
						w.yn1       = (int16_t)be16(t + 0x20);
						w.yn2       = (int16_t)be16(t + 0x22);
						waves.push_back(w);
					}
				}
			}
			i++;
		} else {
			if (c == 7)
				fxScene.push_back(be32(&aaf[i * 4]));
			i += 3;
		}
	}
	printf("%zu waves in the WSYS tables\n", waves.size());

	// Load the wave archives the selected waves live in.
	std::vector<std::string> loaded;
	std::vector<uint32_t> loadedAt;
	uint32_t aramTop = 0x4000;
	auto arcBase = [&](const std::string& name) -> int64_t {
		for (size_t k = 0; k < loaded.size(); k++)
			if (loaded[k] == name)
				return loadedAt[k];
		Bytes aw;
		if (!read_file(disc + "/AudioRes/Banks/" + name, aw) || aramTop + aw.size() > g_aram.size())
			return -1;
		memcpy(&g_aram[aramTop], aw.data(), aw.size());
		loaded.push_back(name);
		loadedAt.push_back(aramTop);
		uint32_t at = aramTop;
		aramTop     = (aramTop + (uint32_t)aw.size() + 31) & ~31u;
		return at;
	};

	port_dspmix_set_aram(aram_at);
	port_dspmix_setup(64, g_voices, NULL, NULL, g_fx);
	port_dspmix_set_mram(resolve_fx);

	// Decoder check over every archive: decoding a looped wave from its start
	// must reproduce the ADPCM history the wave table stores for the loop
	// start (loopYN1 = y[loopStart-1], loopYN2 = y[loopStart-2]).
	{
		std::vector<std::string> names;
		for (size_t k = 0; k < waves.size(); k++)
			if (std::find(names.begin(), names.end(), waves[k].arc) == names.end())
				names.push_back(waves[k].arc);
		int ok[4] = {}, bad[4] = {}, rails[4] = {}, total[4] = {};
		for (size_t a = 0; a < names.size(); a++) {
			Bytes aw;
			if (!read_file(disc + "/AudioRes/Banks/" + names[a], aw))
				continue;
			if (aw.size() > g_aram.size())
				g_aram.resize(aw.size());
			memcpy(&g_aram[0], aw.data(), aw.size());
			for (size_t k = 0; k < waves.size(); k++) {
				const Wave& w = waves[k];
				if (w.arc != names[a] || w.offset + w.size > aw.size())
					continue;
				int f = w.format & 3;
				// Looped waves are stored only up to the loop end (the table's
				// sample count can be larger): decode what the data holds.
				static const uint32_t kBytes[4] = { 9, 5, 16, 32 };
				uint32_t n = std::min<uint32_t>(w.count, w.size / kBytes[f] * 16);
				std::vector<int16_t> dec(n);
				port_dspmix_decode_wave(w.format, w.offset, (int)n, dec.data());
				for (size_t s = 0; s < dec.size(); s++)
					rails[f] += dec[s] == 32767 || dec[s] == -32768;
				total[f] += (int)n;
				uint32_t fs = w.loopStart & ~15u; // history is kept for the loop start's frame
				if (w.loop && f < 2 && fs <= n) {
					bool match = fs >= 2 ? dec[fs - 1] == w.yn1 && dec[fs - 2] == w.yn2 : w.yn1 == 0 && w.yn2 == 0;
					if (match)
						ok[f]++;
					else if (bad[f]++ < 3)
						printf("  loop history mismatch %s #%d fmt %d: got %d,%d want %d,%d\n", w.arc.c_str(),
						       w.index, w.format, fs >= 1 ? dec[fs - 1] : 0, fs >= 2 ? dec[fs - 2] : 0, w.yn1, w.yn2);
				}
			}
		}
		for (int f = 0; f < 4; f++)
			if (total[f])
				printf("format %d: %d samples, %.4f%% at the rails; loop history %d match, %d differ\n", f,
				       total[f], 100.0 * rails[f] / total[f], ok[f], bad[f]);
		memset(&g_aram[0], 0, g_aram.size());
	}

	// Pick: the first few one-shot and looping waves of each format from the
	// small archives, plus decode sanity statistics over whole archives.
	int picked[4][2] = {};
	int written      = 0;
	for (size_t k = 0; k < waves.size(); k++) {
		const Wave& w = waves[k];
		if (w.arc != "w1stLoad_0.aw" && w.arc != "wScene_0.aw")
			continue;
		int64_t ab = arcBase(w.arc);
		if (ab < 0)
			continue;
		uint32_t base = (uint32_t)ab + w.offset;
		int f = w.format & 3, l = w.loop ? 1 : 0;
		if (picked[f][l] >= 2 || w.count < 2000)
			continue;
		picked[f][l]++;
		double ratio = w.rate / 32028.5;
		setup_voice(g_voices[0], w, base, ratio, 0x16A0, 0x16A0, 0);
		Render r = render(w.loop ? 3.0 : 10.0, w.loop ? 2.0 : 0);
		char name[256];
		snprintf(name, sizeof name, "%s/%s_%03d_fmt%d%s.wav", out.c_str(), w.arc.c_str(), w.index, w.format,
		         w.loop ? "_loop" : "");
		write_wav(name, r.pcm);
		written++;
		double expect = w.loop ? 2.0 : w.count / (double)w.rate;
		printf("%-40s rate %5.0f key %3d n %7u loop %d [%u,%u)  rendered %.3fs (source %.3fs)  peak %5d rms %6.0f\n",
		       name + out.size() + 1, w.rate, w.key, w.count, (int)w.loop, w.loopStart, w.loopEnd,
		       r.subframes * 80 / 32000.0, expect, r.peak, r.rms);
	}

	// Pitch: the first one-shot of w1stLoad an octave up (half the length)
	// and an octave down, as a sequence note would (C5BASE_PITCHTABLE).
	for (size_t k = 0; k < waves.size(); k++) {
		const Wave& w = waves[k];
		if (w.arc != "wScene_0.aw" || w.loop || w.count < 8000)
			continue;
		uint32_t base = (uint32_t)arcBase(w.arc) + w.offset;
		for (int oct = -1; oct <= 1; oct += 2) {
			double ratio = w.rate / 32028.5 * pow(2.0, oct);
			setup_voice(g_voices[0], w, base, ratio, 0x16A0, 0x16A0, 0);
			Render r = render(10.0, 0);
			char name[256];
			snprintf(name, sizeof name, "%s/note_%s_%03d_oct%+d.wav", out.c_str(), w.arc.c_str(), w.index, oct);
			write_wav(name, r.pcm);
			printf("%-40s octave %+d: rendered %.3fs, expected %.3fs\n", name + out.size() + 1, oct,
			       r.subframes * 80 / 32000.0, w.count / (double)w.rate / pow(2.0, oct));
		}
		break;
	}

	// FX: the same note with full fx send through FX line 0/1 configured as
	// scene 0 of the AAF's FX table (JAIData::initFxline -> setFXLine).
	if (!fxScene.empty()) {
		static const uint16_t kSend[12] = { 0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80, 0x0EE0,
			                                0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0, 0x0000 };
		const uint8_t* t  = &aaf[fxScene[0]];
		const uint8_t* sc = t + be32(t + 0x14);
		for (int l = 0; l < 4; l++) {
			const uint8_t* c = sc + l * 0x20;
			uint8_t* f       = g_fx[l];
			memset(f, 0, 0x20);
			put<uint16_t>(f, 0x00, c[0]);
			put<uint16_t>(f, 0x02, (uint16_t)be32(c + 0xC));
			fxMemory[l].assign(be32(c + 0xC) * 80, 0);
			put<uint32_t>(f, 0x04, l + 1); // offline main-memory buffer token
			put<uint16_t>(f, 0x08, kSend[be16(c + 2)]);
			put<int16_t>(f, 0x0A, (int16_t)be16(c + 4));
			put<uint16_t>(f, 0x0C, kSend[be16(c + 6)]);
			put<int16_t>(f, 0x0E, (int16_t)be16(c + 8));
			for (int q = 0; q < 8; q++)
				put<int16_t>(f, 0x10 + q * 2, (int16_t)be16(c + 0x10 + q * 2));
		}
		for (size_t k = 0; k < waves.size(); k++) {
			const Wave& w = waves[k];
			if (w.arc != "wScene_0.aw" || w.loop || w.count < 8000)
				continue;
			uint32_t base = (uint32_t)arcBase(w.arc) + w.offset;
			setup_voice(g_voices[0], w, base, w.rate / 32028.5, 0x16A0, 0x16A0, 127);
			Render r = render(10.0, 0);
			char name[256];
			snprintf(name, sizeof name, "%s/fx_%s_%03d.wav", out.c_str(), w.arc.c_str(), w.index);
			write_wav(name, r.pcm);
			printf("%-40s with FX lines (scene 0): %.3fs, peak %d rms %.0f\n", name + out.size() + 1,
			       r.subframes * 80 / 32000.0, r.peak, r.rms);
			break;
		}
	}
	printf("%d WAV files written to %s\n", written, out.c_str());
	return 0;
}
