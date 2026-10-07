// Audio Interface (AI): the DMA engine that plays JAudio's DAC buffers.
//
// JAudio (JASAiCtrl.cpp / JASAudioThread.cpp) programs one DMA block at a time
// with AIInitDMA; the hardware latches the registers when a block starts
// playing and raises the DMA interrupt, whose callback (syncAudio) wakes the
// audio thread to mix and program the next block. Here a block "starts" when
// the host output FIFO has room for it: the latched block (host-order s16
// stereo pairs, written by the CPU) is copied into the FIFO and the callback
// runs in interrupt context on the emulated CPU.
//
// Output: SDL2 audio (loaded with dlopen, so builds without the GX layer still
// link) at 32 kHz; the SDL callback drains the FIFO and kicks the CPU when it
// runs low. Without a device (headless, SMS_AUDIO_OUT=null, or SDL failure) a
// host timer paces blocks at the DAC rate. SMS_AUDIO_WAV=path also records
// every block. SMS_AUDIO=0 drops the output only: the DMA engine keeps its
// clock (as with no device), so JAudio's audio thread, its sequencer and the
// DSP run as on the console; stopping them would leave every sequence the game
// stops allocated (only the sequencer frees it), and the eight root sequence
// slots run out after eight music changes (README.md, Environment).
// SMS_NO_AUDIO (an empty sound configuration) leaves the DMA engine idle.
// Pairs are (bus 2, bus 1) = (right, left) in DMA order; they are swapped for
// SDL (SMS_AUDIO_SWAP=0 keeps DMA order).
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/ai.h>

#include <atomic>
#include <algorithm>
#include <dlfcn.h>
#include "port_host.h"
#include <mutex>
#include <pthread.h>
#include <time.h>
#include <vector>

extern "C" int port_audio_dsp_frame_pending(void) __attribute__((weak));
extern "C" int GXPC_GetSpeed(void) __attribute__((weak));
extern "C" void port_dtk_mix(s16* lr, size_t frames, u32 outRate);

namespace {

// Minimal SDL2 ABI (public SDL_audio.h): only what the output needs.
struct SdlAudioSpec {
	int freq;
	uint16_t format;
	uint8_t channels;
	uint8_t silence;
	uint16_t samples;
	uint16_t padding;
	uint32_t size;
	void (*callback)(void* userdata, uint8_t* stream, int len);
	void* userdata;
};
const uint32_t kSdlInitAudio = 0x10;
const uint16_t kAudioS16Sys  = 0x8010; // AUDIO_S16LSB (little-endian hosts)

typedef int (*PFN_SDL_InitSubSystem)(uint32_t);
typedef uint32_t (*PFN_SDL_OpenAudioDevice)(const char*, int, const SdlAudioSpec*, SdlAudioSpec*, int);
typedef void (*PFN_SDL_PauseAudioDevice)(uint32_t, int);
typedef const char* (*PFN_SDL_GetError)(void);

const u32 kRate = 32000;

struct Ai {
	bool enabled; // the DMA engine runs (not SMS_NO_AUDIO)
	bool output;  // SMS_AUDIO != 0: a device or SMS_AUDIO_WAV
	bool inited;
	bool running;
	bool swap;
	int volume;                // SMS_VOLUME (0..100) as a Q8 output gain; 256 is unity
	u32 start, length;         // registers
	u32 latchedStart, latchedLen;
	AIDCallback cb;
	u32 dspRate;
	u32 streamRate;
	// Output FIFO of stereo frames (L, R after swap).
	std::mutex mu;
	std::vector<int16_t> fifo;
	size_t head, count;
	bool sdl;
	s16 lastL, lastR;
	// Null clock
	std::atomic<int> due;
	u64 blocks;
	FILE* wav;
	u32 wavBytes;
	std::vector<int16_t> mix; // a block in output order, with the DVD stream (dtk.cpp)
} g;

size_t fifo_cap() { return g.fifo.size() / 2; }

int playback_speed()
{
	int speed = GXPC_GetSpeed ? GXPC_GetSpeed() : 1;
	return speed > 0 ? speed : 1;
}

size_t fifo_target(int speed) { return 3 * 560 * (size_t)speed; }

void wav_header(FILE* f, u32 bytes)
{
	u32 v32;
	u16 v16;
	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f);
	v32 = 36 + bytes;
	fwrite(&v32, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	v32 = 16;
	fwrite(&v32, 4, 1, f);
	v16 = 1;
	fwrite(&v16, 2, 1, f);
	v16 = 2;
	fwrite(&v16, 2, 1, f);
	v32 = kRate;
	fwrite(&v32, 4, 1, f);
	v32 = kRate * 4;
	fwrite(&v32, 4, 1, f);
	v16 = 4;
	fwrite(&v16, 2, 1, f);
	v16 = 16;
	fwrite(&v16, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
	fseek(f, 0, SEEK_END);
}

void wav_close(void)
{
	if (g.wav) {
		wav_header(g.wav, g.wavBytes);
		fclose(g.wav);
		g.wav = NULL;
	}
}

void sdl_callback(void*, uint8_t* stream, int len)
{
	int16_t* out = (int16_t*)stream;
	size_t frames = (size_t)len / 4;
	int speed = playback_speed();
	bool low;
	{
		std::lock_guard<std::mutex> lk(g.mu);
		size_t cap = fifo_cap();
		for (size_t i = 0; i < frames; i++) {
			if (g.count) {
				g.lastL = g.fifo[g.head * 2];
				g.lastR = g.fifo[g.head * 2 + 1];
				size_t consumed = std::min(g.count, (size_t)speed);
				g.head = (g.head + consumed) % cap;
				g.count -= consumed;
			}
			out[i * 2]     = (int16_t)((g.lastL * g.volume) >> 8);
			out[i * 2 + 1] = (int16_t)((g.lastR * g.volume) >> 8);
		}
		low = g.count < fifo_target(speed);
	}
	if (low)
		port_irq_kick();
}

void* null_clock(void*)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	for (;;) {
		u32 len = g.latchedLen ? g.latchedLen : 0x460 * 2;
		long ns = (long)((u64)(len / 4) * 1000000000ull / kRate / playback_speed());
		t.tv_nsec += ns;
		while (t.tv_nsec >= 1000000000) {
			t.tv_nsec -= 1000000000;
			t.tv_sec++;
		}
		port_sleep_until(&t);
		if (g.running) {
			if (g.due.load() < 4) // a stalled game does not get a burst later
				g.due.fetch_add(1);
			port_irq_kick();
		}
	}
	return NULL;
}

bool open_sdl()
{
#ifdef _WIN32
	void* h = dlopen("SDL2.dll", RTLD_NOW | RTLD_GLOBAL);
#elif defined(__APPLE__)
	// Prefer symbols already linked (SDL2.framework), then common dylib names.
	void* h = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	if (!h || !dlsym(h, "SDL_InitSubSystem")) {
		h = dlopen("@rpath/SDL2.framework/SDL2", RTLD_NOW | RTLD_GLOBAL);
	}
	if (!h)
		h = dlopen("SDL2.framework/SDL2", RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		h = dlopen("libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		h = dlopen("libSDL2.dylib", RTLD_NOW | RTLD_GLOBAL);
#else
	void* h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
#endif
	if (!h)
		return false;
	PFN_SDL_InitSubSystem init  = (PFN_SDL_InitSubSystem)dlsym(h, "SDL_InitSubSystem");
	PFN_SDL_OpenAudioDevice op  = (PFN_SDL_OpenAudioDevice)dlsym(h, "SDL_OpenAudioDevice");
	PFN_SDL_PauseAudioDevice pa = (PFN_SDL_PauseAudioDevice)dlsym(h, "SDL_PauseAudioDevice");
	PFN_SDL_GetError err        = (PFN_SDL_GetError)dlsym(h, "SDL_GetError");
	if (!init || !op || !pa || !err)
		return false;
	// SDL turns SIGINT/SIGTERM into an SDL_QUIT event; nothing pumps events in
	// a headless run, so the process would ignore them. (No effect if the GX
	// layer initialised SDL first; the user's own setting wins.)
	port_setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
	if (init(kSdlInitAudio) != 0) {
		port_log("[audio] SDL audio init failed: %s\n", err());
		return false;
	}
	SdlAudioSpec want, have;
	memset(&want, 0, sizeof want);
	want.freq     = kRate;
	want.format   = kAudioS16Sys;
	want.channels = 2;
	want.samples  = 512;
	want.callback = sdl_callback;
	uint32_t dev  = op(NULL, 0, &want, &have, 0); // no changes allowed: SDL converts
	if (!dev) {
		port_log("[audio] no SDL audio device: %s\n", err());
		return false;
	}
	pa(dev, 0);
	port_log("[audio] SDL output: %d Hz, %u-frame device buffer\n", have.freq, have.samples);
	return true;
}

void init_output()
{
	const char* e = getenv("SMS_AUDIO");
	g.output      = !(e && strcmp(e, "0") == 0);
	g.enabled     = !port_no_audio;
	e             = getenv("SMS_AUDIO_SWAP");
	g.swap        = !(e && strcmp(e, "0") == 0);
	e             = getenv("SMS_VOLUME");
	g.volume      = e && *e ? std::max(0, std::min(100, atoi(e))) * 256 / 100 : 256;
	if (g.volume != 256)
		port_log("[audio] master volume %d%%\n", (g.volume * 100 + 128) / 256);
	g.fifo.assign(32768 * 2, 0);
	g.head = g.count = 0;
	if (!g.enabled) {
		port_log("[audio] SMS_NO_AUDIO: AI DMA idle (no mixing, no output)\n");
		return;
	}
	if (!g.output)
		port_log("[audio] SMS_AUDIO=0: no output; the game's audio still runs\n");
	if (g.output && (e = getenv("SMS_AUDIO_WAV")) && *e) {
		g.wav = fopen(e, "wb");
		if (g.wav) {
			wav_header(g.wav, 0);
			atexit(wav_close);
		}
	}
	e              = getenv("SMS_AUDIO_OUT");
	bool headless  = getenv("SMS_HEADLESS") && strcmp(getenv("SMS_HEADLESS"), "0") != 0;
	bool wantSdl   = e ? strcmp(e, "sdl") == 0 : !headless;
	// SMS_VI_DETERMINISTIC: no device and no host clock; AI blocks are paced by
	// VI retraces instead (port_audio_on_retrace), so runs stay repeatable.
	if (port_vi_deterministic()) {
		port_log("[audio] deterministic VI clock: AI DMA paced by retraces, no output device\n");
		return;
	}
	g.sdl          = g.output && wantSdl && open_sdl();
	if (!g.sdl) {
		if (g.output)
			port_log("[audio] no audio device: pacing AI DMA from the host clock\n");
		pthread_t th;
		pthread_create(&th, NULL, null_clock, NULL);
		pthread_detach(th);
	}
}

// One DMA block starts: latch the registers, queue the samples, interrupt.
void dma_block()
{
	g.latchedStart = g.start;
	g.latchedLen   = g.length;
	const int16_t* src = (const int16_t*)(uintptr_t)g.latchedStart;
	size_t frames      = g.latchedLen / 4;
	if (src && frames) {
		g.mix.resize(frames * 2);
		for (size_t i = 0; i < frames; i++) {
			g.mix[i * 2]     = g.swap ? src[i * 2 + 1] : src[i * 2];
			g.mix[i * 2 + 1] = g.swap ? src[i * 2] : src[i * 2 + 1];
		}
		port_dtk_mix(g.mix.data(), frames, kRate);
		if (g.wav) {
			fwrite(g.mix.data(), 4, frames, g.wav);
			g.wavBytes += (u32)frames * 4;
			if ((++g.blocks & 63) == 0) {
				wav_header(g.wav, g.wavBytes);
				fflush(g.wav);
			}
		}
		if (g.sdl) {
			std::lock_guard<std::mutex> lk(g.mu);
			size_t cap = fifo_cap();
			for (size_t i = 0; i < frames && g.count < cap; i++) {
				size_t p         = (g.head + g.count) % cap;
				g.fifo[p * 2]     = g.mix[i * 2];
				g.fifo[p * 2 + 1] = g.mix[i * 2 + 1];
				g.count++;
			}
		}
	}
	if (g.cb)
		g.cb();
}

u64 g_det_acc; // deterministic pacing: samples owed, in 1/60000 units

// Interrupt source, polled on the emulated CPU when kicked.
void ai_poll()
{
	if (!g.running)
		return;
	if (port_audio_dsp_frame_pending && port_audio_dsp_frame_pending()) {
		port_irq_kick(); // the audio thread is mid-frame: retry at the next check point
		return;
	}
	if (g.sdl) {
		size_t level;
		size_t target = fifo_target(playback_speed());
		{
			std::lock_guard<std::mutex> lk(g.mu);
			level = g.count;
		}
		if (level < target) {
			dma_block(); // one block per delivery: the audio thread must run
			std::lock_guard<std::mutex> lk(g.mu);
			if (g.count < target)
				port_irq_kick();
		}
	} else if (g.due.load() > 0) {
		g.due.fetch_sub(1);
		dma_block();
		if (g.due.load() > 0)
			port_irq_kick(); // catch up one block per delivery
	}
}

} // namespace

// Called by platform/vi once per retrace. In deterministic mode each retrace
// owes 32000 * 1001 / 60000 samples; whole DMA blocks become due.
extern "C" void port_audio_on_retrace(void)
{
	if (!port_vi_deterministic() || !g.enabled || !g.running)
		return;
	g_det_acc += (u64)kRate * 1001;
	u32 len      = g.latchedLen ? g.latchedLen : 0x460 * 2;
	u64 perBlock = (u64)(len / 4) * 60000;
	while (g_det_acc >= perBlock) {
		g_det_acc -= perBlock;
		g.due.fetch_add(1);
	}
	if (g.due.load() > 0)
		port_irq_kick();
}

extern "C" void AIInit(u8*)
{
	if (g.inited)
		return;
	g.inited = true;
	init_output();
	port_irq_add_source(ai_poll);
}
extern "C" BOOL AICheckInit(void) { return g.inited; }
extern "C" void AIReset(void) {}
extern "C" AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
	AIDCallback old = g.cb;
	g.cb            = cb;
	return old;
}
extern "C" void AIInitDMA(u32 start_addr, u32 length)
{
	g.start  = start_addr;
	g.length = length;
}
extern "C" BOOL AIGetDMAEnableFlag(void) { return g.running; }
extern "C" void AIStartDMA(void)
{
	if (!g.enabled)
		return;
	g.running = true;
	port_irq_kick();
}
extern "C" void AIStopDMA(void) { g.running = false; }
extern "C" u32 AIGetDMABytesLeft(void) { return 0; }
extern "C" u32 AIGetDMAStartAddr(void) { return g.latchedStart; }
extern "C" u32 AIGetDMALength(void) { return g.latchedLen; }
extern "C" void AISetDSPSampleRate(u32 rate) { g.dspRate = rate; }
extern "C" u32 AIGetDSPSampleRate(void) { return g.dspRate; }
// DVD audio streaming (DTK, dtk.cpp): retail SMS never streams; code mods do.
extern "C" void AISetStreamSampleRate(u32 rate) { g.streamRate = rate; }
extern "C" u32 AIGetStreamSampleRate(void) { return g.streamRate; }
