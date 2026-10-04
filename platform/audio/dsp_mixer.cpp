// Software DSP mixer: renders the voices JAudio describes in its DSPBuffer
// blocks. Layouts and semantics are derived from the decomp's own JAudio
// source (JASDSPInterface.hpp/.cpp, JASChannel.cpp, JASDSPChannel.cpp,
// JASDriverTables.cpp); see PROTOCOL.md for the evidence behind each choice
// and for what is approximated.
#include "dsp_mixer.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------------------
// Game-side blocks (host byte order: the CPU writes them field by field).
// Pointer-typed members in the game header are stored here as u32: they hold
// ARAM addresses or sample indices, never host pointers the mixer follows.

struct MixSlot {
	uint16_t bus;     // DSP bus address (connect_table), 0 = unconnected
	uint16_t target;  // Q15 volume the slot ramps to over one subframe
	uint16_t current; // Q15 volume at the start of the subframe (DSP writes back)
	uint16_t level;   // hi: init delay, lo: delay (surround pan delay; ignored)
};

struct Voice {
	uint16_t enabled;          // 0x000
	uint16_t done;             // 0x002 DSP sets when the voice has finished
	uint16_t ratio;            // 0x004 Q12 resampling ratio (0x1000 = 1:1)
	uint16_t pad006;           // 0x006
	uint16_t resetVpb;         // 0x008 set by playStart: (re)start decoding
	uint16_t endReached;       // 0x00A DSP sets at the end of a one-shot wave
	uint16_t pause;            // 0x00C useConstantSample (setPauseFlag)
	uint16_t delayMax;         // 0x00E setMixerInitDelayMax
	MixSlot slot[6];           // 0x010
	uint8_t pad040[0x10];      // 0x040
	uint16_t amPan;            // 0x050 auto mixer: pan << 8 | dolby
	uint16_t amFx;             // 0x052 auto mixer: fx << 8
	uint16_t amVolCurrent;     // 0x054
	uint16_t amVolTarget;      // 0x056
	uint16_t amEnabled;        // 0x058 useDolbyVolume
	uint8_t pad05A[6];         // 0x05A
	int16_t posFrac;           // 0x060
	uint16_t pad062;           // 0x062
	int16_t blockSamples;      // 0x064
	int16_t constantSample;    // 0x066
	int32_t position;          // 0x068
	uint32_t samplesBeforeLoop;// 0x06C
	uint32_t aramAddr;         // 0x070
	uint32_t remaining;        // 0x074
	int16_t resampleHist[4];   // 0x078
	uint16_t firHist[20];      // 0x080
	int16_t iirHist[4];        // 0x0A8
	uint16_t afcSamples[16];   // 0x0B0
	int16_t lpHist[2];         // 0x0D0
	uint8_t pad0D4[0x2C];      // 0x0D4
	uint16_t sourceType;       // 0x100 9 ADPCM4, 5 ADPCM2, 8 PCM8, 16 PCM16, <4 oscillator
	uint16_t looping;          // 0x102
	int16_t loopYN1;           // 0x104
	int16_t loopYN2;           // 0x106
	int16_t filterMode;        // 0x108
	uint16_t endRequested;     // 0x10A
	uint32_t cpuTicks;         // 0x10C (CPU-owned age counter)
	uint32_t loopStart;        // 0x110 sample index (stored through an s16*)
	uint32_t endPosition;      // 0x114 loop end, or sample count
	uint32_t base;             // 0x118 ARAM byte address of the wave
	int32_t sampleCount;       // 0x11C
	int16_t fir[20];           // 0x120
	int16_t iir[4];            // 0x148
	int16_t lowPass;           // 0x150
	uint8_t pad152[0x2E];      // 0x152
};
static_assert(sizeof(Voice) == 0x180, "DSPBuffer is 0x180 bytes");
static_assert(offsetof(Voice, slot) == 0x10, "mixChannels");
static_assert(offsetof(Voice, amPan) == 0x50, "dolbyVoicePosition");
static_assert(offsetof(Voice, posFrac) == 0x60, "currentPosFrac");
static_assert(offsetof(Voice, sourceType) == 0x100, "samplesSourceType");
static_assert(offsetof(Voice, endRequested) == 0x10A, "endRequested");
static_assert(offsetof(Voice, base) == 0x118, "baseAddress");
static_assert(offsetof(Voice, fir) == 0x120, "variableFirCoeffs");

struct FxLine {
	uint16_t mode;      // 0x00 0 = off (config unk0)
	uint16_t length;    // 0x02 delay length in subframes (config unkC)
	uint32_t buffer;    // 0x04 main-memory delay buffer (non-zero = active)
	uint16_t busA;      // 0x08 SEND_TABLE[config unk2]
	int16_t gainA;      // 0x0A Q15
	uint16_t busB;      // 0x0C SEND_TABLE[config unk6]
	int16_t gainB;      // 0x0E Q15
	int16_t taps[8];    // 0x10 Q15 FIR
};
static_assert(sizeof(FxLine) == 0x20, "FXBuffer is 0x20 bytes");

// ---------------------------------------------------------------------------
// Tables

// DSPADPCM_FILTER from the decomp (JASDriverTables.cpp): the AFC predictor
// pairs, Q11. Used when the setup command does not provide a table.
const uint32_t kAdpcmDefault[16] = {
	0x0,        0x08000000, 0x00000800, 0x04000400, 0x1000f800, 0x0e00fa00, 0x0c00fc00, 0x1200f600,
	0x1068f738, 0x12c0f704, 0x1400f400, 0x0800f800, 0x0400fc00, 0xfc000400, 0xfc000000, 0xf8000000,
};

enum { kBuses = 12, kMaxN = 1024, kDirectPcm = 0x21 };

// Bus addresses in DSP DRAM, indexed as JASDSPInterface's connect_table.
const uint16_t kBusAddr[kBuses] = { 0x0000, 0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80,
	                                0x0EE0, 0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0 };

int bus_index(uint16_t addr)
{
	for (int i = 1; i < kBuses; i++)
		if (kBusAddr[i] == addr)
			return i;
	return 0;
}

struct Decoder {
	bool active;
	bool ended;       // one-shot source ran out
	int tail;         // samples pulled past the end (window flush)
	uint32_t idx;     // next source sample to decode
	int32_t h1, h2;   // ADPCM history
	uint32_t frame;   // cached frame header index (+1; 0 = none)
	uint8_t header;
	uint32_t remaining; // direct PCM: samples left to pull
	uint32_t written;   // remainingLength as last written back
	uint32_t startTick; // trace: subframe the voice started
	int16_t win[4];   // x[n-1], x[n], x[n+1], x[n+2]
	uint32_t frac;    // Q16 position between win[1] and win[2]
};

struct State {
	Voice* voices;
	uint32_t nvoices;
	FxLine* fx;
	int16_t res[64 * 4];
	int16_t coef[16][2];
	port_dspmix_aram_fn aram;
	Decoder dec[64];
	int32_t bus[kBuses][kMaxN];
	port_dspmix_mram_fn mram;
	uint32_t fxBuffer[4];
	uint32_t fxLength[4];
	uint32_t fxPos[4];
	int32_t fxHist[4][8];
	int active;
	int masterShift; // master level fixed point: Q15 (measured against retail, PROTOCOL.md)
	int slotShift;   // mixChannels volume fixed point: Q14
	bool fxOff;
	bool trace;
	uint32_t traceTick;
	bool warnedOsc;
} g;

void build_default_resampler()
{
	// Catmull-Rom weights for x[n-1..n+2], Q15. Only used without the game's
	// DSPRES_FILTER (the WAV test).
	for (int p = 0; p < 64; p++) {
		double t = p / 64.0, t2 = t * t, t3 = t2 * t;
		double w[4] = { -0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t,
			            0.5 * t3 - 0.5 * t2 };
		for (int k = 0; k < 4; k++)
			g.res[p * 4 + k] = (int16_t)lrint(w[k] * 32767.0);
	}
}

void load_adpcm_table(const uint32_t* t)
{
	for (int i = 0; i < 16; i++) {
		g.coef[i][0] = (int16_t)(t[i] >> 16);
		g.coef[i][1] = (int16_t)(t[i] & 0xFFFF);
	}
}

inline int32_t sat16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

const uint8_t* aram(uint32_t addr) { return g.aram ? g.aram(addr) : NULL; }

// ---------------------------------------------------------------------------
// Source decoding (one sample at a time; ADPCM frames are 16 samples).

int32_t decode_at(Decoder& d, const Voice& v)
{
	uint32_t i = d.idx;
	switch (v.sourceType) {
	case 9:
	case 5: {
		// AFC ADPCM: per 16-sample frame one header byte (high nibble: scale
		// exponent, low nibble: predictor index into the ADPCM table), then
		// 16 signed 4-bit (9-byte frames) or 2-bit (5-byte frames) residuals.
		const uint32_t fbytes = v.sourceType;
		uint32_t f            = i >> 4;
		const uint8_t* p      = aram(v.base + f * fbytes);
		if (!p)
			return 0;
		int k = i & 15, r;
		if (fbytes == 9) {
			uint8_t b = p[1 + (k >> 1)];
			r         = (k & 1) ? (b & 0xF) : (b >> 4);
			r         = (r ^ 8) - 8;
		} else {
			uint8_t b = p[1 + (k >> 2)];
			r         = (b >> (6 - 2 * (k & 3))) & 3;
			r         = ((r ^ 2) - 2) * 4; // 2-bit residual on the 4-bit scale
		}
		int scale = p[0] >> 4, pi = p[0] & 0xF;
		int32_t s = ((int32_t)(r << scale) << 11) + g.coef[pi][0] * d.h1 + g.coef[pi][1] * d.h2;
		s         = sat16(s >> 11);
		d.h2      = d.h1;
		d.h1      = s;
		return s;
	}
	case 8: {
		const uint8_t* p = aram(v.base + i);
		return p ? (int8_t)p[0] * 256 : 0;
	}
	case 16: {
		const uint8_t* p = aram(v.base + i * 2); // PCM16 waves are big-endian in ARAM
		return p ? (int16_t)((p[0] << 8) | p[1]) : 0;
	}
	case kDirectPcm:
		// Stream voices (JAInter::StreamLib::Play_DirectPCM): a ring of host-
		// order s16 in main memory, filled by the CPU's AFC decoder.
		return v.base ? ((const int16_t*)(uintptr_t)v.base)[i] : 0;
	}
	return 0;
}

int32_t pull(Decoder& d, const Voice& v)
{
	if (d.ended) {
		d.tail++;
		return 0;
	}
	if (v.sourceType == kDirectPcm) {
		// endPosition is the ring length in 16.16; remainingLength counts the
		// samples still to play (0xFFFFFFFF: endless, a looping stream).
		uint32_t ring = v.endPosition >> 16;
		if (d.remaining == 0 || !ring) {
			d.ended = true;
			d.tail  = 1;
			return 0;
		}
		if (d.remaining != 0xFFFFFFFFu)
			d.remaining--;
		if (d.idx >= ring)
			d.idx = 0;
		int32_t s = decode_at(d, v);
		d.idx++;
		return s;
	}
	if (d.idx >= v.endPosition) {
		if (v.looping && v.loopStart < v.endPosition) {
			if (v.sourceType == 9 || v.sourceType == 5) {
				// The stored loop history is y[-1], y[-2] of the ADPCM frame
				// that holds the loop start (verified against every looped
				// wave on the disc: tests/audio_test). Resume at that frame
				// and decode up to the loop start so the period stays exact.
				d.idx = v.loopStart & ~15u;
				d.h1  = v.loopYN1;
				d.h2  = v.loopYN2;
				while (d.idx < v.loopStart) {
					decode_at(d, v);
					d.idx++;
				}
			} else {
				d.idx = v.loopStart;
			}
		} else {
			d.ended = true;
			d.tail  = 1;
			return 0;
		}
	}
	int32_t s = decode_at(d, v);
	d.idx++;
	return s;
}

void start(Decoder& d, const Voice& v)
{
	memset(&d, 0, sizeof d);
	d.active    = true;
	d.remaining = v.remaining;
	d.written   = v.remaining;
	d.win[0] = 0;
	for (int k = 1; k < 4; k++)
		d.win[k] = (int16_t)pull(d, v);
}

bool supported(uint16_t type) { return type == 9 || type == 5 || type == 8 || type == 16 || type == kDirectPcm; }

// Resample `n` samples of voice `v` into out; returns the number produced
// before a one-shot source ran dry (the rest is zero-filled).
int resample(Decoder& d, const Voice& v, int32_t* out, int n)
{
	uint32_t step = (uint32_t)v.ratio << 4; // Q12 -> Q16
	int k;
	for (k = 0; k < n; k++) {
		if (d.ended && d.tail > 2)
			break;
		const int16_t* c = &g.res[(d.frac >> 10) * 4];
		out[k]           = (c[0] * d.win[0] + c[1] * d.win[1] + c[2] * d.win[2] + c[3] * d.win[3]) >> 15;
		d.frac += step;
		while (d.frac >= 0x10000) {
			d.frac -= 0x10000;
			d.win[0] = d.win[1];
			d.win[1] = d.win[2];
			d.win[2] = d.win[3];
			d.win[3] = (int16_t)pull(d, v);
		}
	}
	for (int j = k; j < n; j++)
		out[j] = 0;
	return k;
}

// `shift` is the volume's fixed point: 14 for mixChannels sends (JAudio caps
// them at MAX_MIXERLEVEL = gain x 16384), 15 for the auto mixer
// (volume x 32767.5). Both measured against retail (PROTOCOL.md, Levels).
void mix_slot(int32_t* bus, const int32_t* src, int n, int32_t cur, int32_t tgt, int shift)
{
	if (cur == 0 && tgt == 0)
		return;
	for (int k = 0; k < n; k++) {
		int32_t vol = cur + (tgt - cur) * (k + 1) / n;
		bus[k] += (int32_t)(((int64_t)src[k] * vol) >> shift);
	}
}

void render_voice(int vi, int n)
{
	Voice& v   = g.voices[vi];
	Decoder& d = g.dec[vi];
	if (v.resetVpb) {
		v.resetVpb = 0;
		d.active   = false;
		if (g.trace)
			fprintf(stderr,
			        "[audio] voice %2d start: src %2u base 0x%06x ratio 0x%04x loop %u [%u,%u) n %d vol "
			        "%04x/%04x/%04x/%04x bus %03x/%03x/%03x/%03x am %u\n",
			        vi, v.sourceType, v.base, v.ratio, v.looping, v.loopStart, v.endPosition, v.sampleCount,
			        v.slot[0].target, v.slot[1].target, v.slot[2].target, v.slot[3].target, v.slot[0].bus,
			        v.slot[1].bus, v.slot[2].bus, v.slot[3].bus, v.amEnabled);
	}
	bool stopping = v.endRequested != 0;
	if (!supported(v.sourceType)) {
		if (!g.warnedOsc) {
			g.warnedOsc = true;
			fprintf(stderr, "[audio] oscillator voices (source type %u) are not synthesised yet\n", v.sourceType);
		}
		if (stopping)
			v.done = 1;
		return;
	}
	if (!d.active) {
		start(d, v);
		d.startTick = g.traceTick;
	} else if (v.sourceType == kDirectPcm && v.remaining != d.written)
		d.remaining = v.remaining;
	d.written   = v.remaining; // the CPU rewrote the count (endless streams)
	if (v.pause) {
		if (stopping)
			v.done = 1;
		return;
	}
	int32_t src[kMaxN];
	int got = resample(d, v, src, n);
	g.active++;

	if (v.amEnabled) {
		// Auto mixer (mix config 0xFFFF): volume ramp, sine-law pan, fx send.
		int32_t cur = v.amVolCurrent, tgt = stopping ? cur / 2 : v.amVolTarget;
		if (stopping) {
			v.amVolTarget = (uint16_t)tgt;
			v.done = tgt == 0;
		}
		// Sunshine's early Zelda DSP uses a Q16 auto-mixer reverb factor.
		// Normalising its high byte to 127 doubled the wet send (and made
		// short footsteps leave an unusually prominent ringing tail).
		double pan = (v.amPan >> 8) / 127.0, fx = v.amFx / 65536.0;
		double gl = cos(pan * M_PI / 2), gr = sin(pan * M_PI / 2);
		int32_t lc = (int32_t)(cur * gl), lt = (int32_t)(tgt * gl);
		int32_t rc = (int32_t)(cur * gr), rt = (int32_t)(tgt * gr);
		mix_slot(g.bus[1], src, n, lc, lt, 15);
		mix_slot(g.bus[2], src, n, rc, rt, 15);
		mix_slot(g.bus[5], src, n, (int32_t)(lc * fx), (int32_t)(lt * fx), 15);
		mix_slot(g.bus[6], src, n, (int32_t)(rc * fx), (int32_t)(rt * fx), 15);
		v.amVolCurrent = (uint16_t)tgt;
	} else {
		bool released = true;
		for (int s = 0; s < 6; s++) {
			MixSlot& m  = v.slot[s];
			int b       = bus_index(m.bus);
			int32_t tgt = stopping ? (int16_t)m.current / 2 : (int16_t)m.target;
			if (stopping) {
				m.target = (uint16_t)tgt;
				released = released && tgt == 0;
			}
			if (b)
				mix_slot(g.bus[b], src, n, (int16_t)m.current, tgt, g.slotShift);
			m.current = (uint16_t)tgt;
		}
		if (stopping)
			v.done = released;
	}
	if (v.sourceType == kDirectPcm) {
		// The CPU paces its decoder from these (Get_DirectPCM_*): position of
		// the sample being played, samples left before the ring wraps, and
		// samples left to play. The window reads 3 samples ahead.
		uint32_t ring   = v.endPosition >> 16;
		uint32_t center = ring ? (d.idx + ring * 2 - 3) % ring : 0;
		v.position          = (int32_t)(center << 16);
		v.samplesBeforeLoop = (ring - center) << 16;
		if (v.remaining != 0xFFFFFFFFu)
			v.remaining = d.ended ? 0 : d.remaining + 3;
		d.written = v.remaining;
	} else {
		v.position = (int32_t)d.idx;
	}
	v.posFrac = (int16_t)(d.frac & 0xFFFF);
	if (got < n || (d.ended && d.tail > 2)) {
		v.endReached = 1;
		v.done       = 1;
	}
	if (v.done && g.trace)
		fprintf(stderr, "[audio] voice %2d end: %s after %u subframes at sample %u\n", vi,
		        stopping ? "stop request" : "source end", g.traceTick - d.startTick, d.idx);
}

// Reverb is prepared before voices, then their wet sends are written back to
// the game's circular buffers. This lets CPU clears (setFXLine/stage changes)
// take effect instead of keeping an unrelated private copy of the delay.
int16_t* fx_memory(const FxLine& f, uint32_t len)
{
	return g.mram ? g.mram(f.buffer, len) : (int16_t*)(uintptr_t)f.buffer;
}

void prepare_fx(int n)
{
	if (!g.fx || g.fxOff)
		return;
	for (int i = 0; i < 4; i++) {
		const FxLine& f = g.fx[i];
		uint32_t len = (uint32_t)f.length * 80;
		if (!f.mode || !f.buffer || !len) {
			g.fxBuffer[i] = 0;
			continue;
		}
		if (g.fxBuffer[i] != f.buffer || g.fxLength[i] != len) {
			g.fxBuffer[i] = f.buffer;
			g.fxLength[i] = len;
			g.fxPos[i] = 0;
			memset(g.fxHist[i], 0, sizeof g.fxHist[i]);
		}
		const int16_t* delay = fx_memory(f, len);
		if (!delay)
			continue;
		int32_t window[kMaxN + 8];
		memcpy(window, g.fxHist[i], sizeof g.fxHist[i]);
		for (int k = 0; k < n; k++)
			window[k + 8] = delay[(g.fxPos[i] + k) % len];
		memcpy(g.fxHist[i], window + n, sizeof g.fxHist[i]);
		// The history is chronological. Mode bit 0 filters the return; bit
		// 1 filters the feedback after the return has been mixed.
		for (int pass = 0; pass < 2; pass++) {
			if (f.mode & (1 << pass)) {
				for (int k = 0; k < n; k++) {
					int64_t sum = 0;
					for (int t = 0; t < 8; t++)
						sum += (int64_t)window[k + t] * f.taps[t];
					window[k] = sat16((int32_t)(sum >> 15));
				}
			}
			if (pass == 0) {
				int a = bus_index(f.busA), b = bus_index(f.busB);
				for (int k = 0; k < n; k++) {
					if (a)
						g.bus[a][k] += (window[k] * f.gainA) >> 15;
					if (b)
						g.bus[b][k] += (window[k] * f.gainB) >> 15;
				}
			}
		}
		memcpy(g.bus[3 + i], window, sizeof(int32_t) * n);
	}
}

void finish_fx(int n)
{
	if (!g.fx || g.fxOff)
		return;
	for (int i = 0; i < 4; i++) {
		const FxLine& f = g.fx[i];
		uint32_t len = (uint32_t)f.length * 80;
		if (!f.mode || !f.buffer || !len)
			continue;
		int16_t* delay = fx_memory(f, len);
		if (!delay)
			continue;
		for (int k = 0; k < n; k++)
			delay[(g.fxPos[i] + k) % len] = (int16_t)sat16(g.bus[3 + i][k]);
		g.fxPos[i] = (g.fxPos[i] + n) % len;
	}
}

} // namespace

extern "C" void port_dspmix_setup(uint32_t nvoices, void* voices, const uint32_t* res_filter,
                                  const uint32_t* adpcm_filter, void* fx_lines)
{
	g.voices  = (Voice*)voices;
	g.nvoices = nvoices > 64 ? 64 : nvoices;
	g.fx      = (FxLine*)fx_lines;
	if (res_filter) {
		for (int i = 0; i < 128; i++) {
			g.res[i * 2]     = (int16_t)(res_filter[i] >> 16);
			g.res[i * 2 + 1] = (int16_t)(res_filter[i] & 0xFFFF);
		}
	} else {
		build_default_resampler();
	}
	load_adpcm_table(adpcm_filter ? adpcm_filter : kAdpcmDefault);
	memset(g.dec, 0, sizeof g.dec);
	memset(g.fxBuffer, 0, sizeof g.fxBuffer);
	memset(g.fxHist, 0, sizeof g.fxHist);
	const char* e = getenv("SMS_AUDIO_FX");
	g.fxOff       = e && strcmp(e, "0") == 0;
	e             = getenv("SMS_AUDIO_MASTER_SHIFT");
	g.masterShift = e && *e ? atoi(e) : 15;
	e             = getenv("SMS_AUDIO_SLOT_SHIFT");
	g.slotShift   = e && *e ? atoi(e) : 14;
	e             = getenv("SMS_AUDIO_TRACE");
	g.trace       = e && *e && strcmp(e, "0") != 0;
}

extern "C" void port_dspmix_set_aram(port_dspmix_aram_fn fn) { g.aram = fn; }
extern "C" void port_dspmix_set_mram(port_dspmix_mram_fn fn) { g.mram = fn; }

extern "C" void port_dspmix_render(int16_t* outA, int16_t* outB, int n, uint16_t master)
{
	if (n > kMaxN)
		n = kMaxN;
	for (int b = 0; b < kBuses; b++)
		memset(g.bus[b], 0, sizeof(int32_t) * n);
	g.active = 0;
	prepare_fx(n);
	if (g.voices) {
		for (uint32_t i = 0; i < g.nvoices; i++) {
			Voice& v = g.voices[i];
			if (!v.enabled || v.done) {
				if (!v.enabled)
					g.dec[i].active = false;
				continue;
			}
			render_voice((int)i, n);
		}
	}
	++g.traceTick;
	if (g.trace && g.traceTick % 2000 == 0) { // every 5 s of 80-sample subframes
		int en = 0, stop = 0, silent = 0;
		for (uint32_t i = 0; g.voices && i < g.nvoices; i++) {
			const Voice& v = g.voices[i];
			if (!v.enabled)
				continue;
			en++;
			stop += v.endRequested != 0;
			bool quiet = v.amEnabled ? v.amVolTarget == 0 : true;
			for (int s = 0; !v.amEnabled && s < 6; s++)
				quiet = quiet && (v.slot[s].target == 0 || !bus_index(v.slot[s].bus));
			silent += quiet;
		}
		fprintf(stderr, "[audio] %u subframes: %d voices mixed, %d enabled, %d stopping, %d at zero volume\n",
		        g.traceTick, g.active, en, stop, silent);
	}
	finish_fx(n);
	for (int k = 0; k < n; k++) {
		outA[k] = (int16_t)sat16((int32_t)(((int64_t)g.bus[1][k] * master) >> g.masterShift));
		outB[k] = (int16_t)sat16((int32_t)(((int64_t)g.bus[2][k] * master) >> g.masterShift));
	}
}

extern "C" int port_dspmix_active_voices(void) { return g.active; }

extern "C" void port_dspmix_decode_wave(int format, uint32_t base, int count, int16_t* out)
{
	static const uint16_t types[4] = { 9, 5, 8, 16 };
	Voice v;
	memset(&v, 0, sizeof v);
	v.sourceType  = types[format & 3];
	v.base        = base;
	v.endPosition = (uint32_t)count;
	Decoder d;
	memset(&d, 0, sizeof d);
	if (!g.coef[1][0])
		load_adpcm_table(kAdpcmDefault);
	for (int i = 0; i < count; i++)
		out[i] = (int16_t)pull(d, v);
}
