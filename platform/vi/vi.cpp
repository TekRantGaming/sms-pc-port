// VI: a host timer raises retraces at 59.94 Hz (119.88 for 120 fps). Frame
// buffers are recorded for the GX/present layer; nothing is scanned out here.
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include "port_framerate.h"
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <time.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <sys/stat.h>
#include "port_host.h"

// Frame capture (SMS_SHOTS=field,field,... SMS_SHOT_DIR=dir): the XFB handed to
// VISetNextFrameBuffer is read back through the GX layer at the first VIFlush
// on or after each listed retrace ("field") count, and written as
// <dir>/field<NNNNN>.ppm (tools/shots.py converts and compares). Fields match
// the retail capture numbering (VI retraces since boot).
extern "C" __attribute__((weak)) int GXPC_ReadXFB(const void* xfb, void* rgba, int* w, int* h);
extern "C" __attribute__((weak)) u32 GXPC_FrameCount(void);
extern "C" __attribute__((weak)) void GXPC_SetIdleClock(double (*idleSeconds)(void));
extern "C" __attribute__((weak)) void GXPC_SkipNextPresent(int skip);
extern "C" __attribute__((weak)) double GXPC_LastPresentSeconds(void);
extern "C" double port_idle_seconds(void);
extern "C" void port_pad_autopress_field(u32 field);
extern "C" __attribute__((weak)) void port_trace_on_retrace(uint32_t retrace_count);
extern "C" __attribute__((weak)) void port_audio_on_retrace(void);

namespace {
std::atomic<u32> g_pending(0);
u32 g_retrace_count;
int g_retrace_multiplier = 1;
OSThreadQueue g_retrace_queue;
VIRetraceCallback g_pre, g_post;
void* g_next_fb;
void* g_cur_fb;
bool g_black = true;

// SMS_VI_DETERMINISTIC=1: no wall clock. A retrace happens only when every
// game thread is blocked (the CPU would idle until the next retrace), or when
// the game polls VIGetRetraceCount without making progress; OSGetTime /
// OSGetTick advance with the retrace count (plus a small step per call inside
// a field). Movie-driven runs are then repeatable: the AI DMA is paced by the
// retraces too (port_audio_on_retrace), with or without SMS_AUDIO=0.
bool g_det;
u32 g_det_polls;
u32 g_det_time_calls;
const s64 kTicksPerField = 40500000LL * 1001 / 60000; // 675675
std::vector<u32> g_shots;
size_t g_next_shot;
std::string g_shot_dir;
pthread_t g_gx_thread;
long g_period_ns = 16683350; // the retrace period (timer_thread)

double seconds_now()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

// The clock for SMS_SHOTS / SMS_AUTOPRESS "fields". By default it is game
// time: two fields per copy at 30 fps, one at 60, and half at 120, so captures
// line up with retail even when host rendering runs slower than real time.
// SMS_FIELD_CLOCK=retrace uses elapsed native fields (wall-clock) instead.
bool g_clock_retrace;
u32 g_field_base; // SMS_VI_FIELD_BASE
u32 g_last_frame;
u64 g_game_half_fields;
u32 game_field()
{
	if (g_clock_retrace || g_det || !GXPC_FrameCount)
		return g_retrace_count / g_retrace_multiplier;
	u32 frame = GXPC_FrameCount();
	g_game_half_fields += (u64)(frame - g_last_frame) * (120 / port_active_frame_rate);
	g_last_frame = frame;
	return g_field_base + (u32)(g_game_half_fields / 2);
}

void shots_init()
{
	if (const char* c = getenv("SMS_FIELD_CLOCK"))
		g_clock_retrace = strcmp(c, "retrace") == 0;
	g_gx_thread = pthread_self();
	const char* e = getenv("SMS_SHOTS");
	if (!e || !*e)
		return;
	for (const char* p = e; *p;) {
		g_shots.push_back((u32)strtoul(p, (char**)&p, 10));
		while (*p == ',' || *p == ' ')
			p++;
	}
	std::sort(g_shots.begin(), g_shots.end());
	const char* d = getenv("SMS_SHOT_DIR");
	g_shot_dir    = d ? d : "shots";
	port_mkdir(g_shot_dir.c_str(), 0777);
}

void shots_poll(u32 field, void* xfb)
{
	if (g_next_shot >= g_shots.size() || field < g_shots[g_next_shot] || !xfb || !GXPC_ReadXFB)
		return;
	if (!pthread_equal(pthread_self(), g_gx_thread))
		return; // GL calls only from the GX thread
	int w = 0, h = 0;
	if (!GXPC_ReadXFB(xfb, NULL, &w, &h) || w <= 0 || h <= 0)
		return;
	std::vector<u8> px((size_t)w * h * 4);
	GXPC_ReadXFB(xfb, px.data(), &w, &h);
	char path[1024];
	snprintf(path, sizeof path, "%s/field%05u.ppm", g_shot_dir.c_str(), g_shots[g_next_shot]);
	FILE* f = fopen(path, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int i = 0; i < w * h; i++)
			fwrite(&px[(size_t)i * 4], 1, 3, f);
		fclose(f);
		port_log("[vi] captured field %u (game field %u, retrace %u, gx frame %u) -> %s\n", g_shots[g_next_shot],
		         field, g_retrace_count, GXPC_FrameCount ? GXPC_FrameCount() : 0, path);
	}
	while (g_next_shot < g_shots.size() && g_shots[g_next_shot] <= field)
		g_next_shot++;
}

// The debug overlay's speed multiplier (sms_gx; absent in builds without it).
extern "C" int GXPC_GetSpeed(void) __attribute__((weak));

void* timer_thread(void*)
{
	// SMS_VI_HZ=<rate> overrides the actual retrace rate (benchmarking).
	long period_ns = 16683350 / g_retrace_multiplier; // 59.94 or 119.88 Hz
	if (const char* hz = getenv("SMS_VI_HZ"))
		if (atof(hz) > 0)
			period_ns = (long)(1e9 / atof(hz));
	g_period_ns = period_ns;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	for (;;) {
		int speed = GXPC_GetSpeed ? GXPC_GetSpeed() : 1;
		t.tv_nsec += period_ns / (speed > 0 ? speed : 1);
		while (t.tv_nsec >= 1000000000) {
			t.tv_nsec -= 1000000000;
			t.tv_sec++;
		}
		port_sleep_until(&t);
		g_pending.fetch_add(1);
		port_irq_kick();
	}
	return NULL;
}

void retrace_irq()
{
	u32 n = g_pending.exchange(0);
	if (n == 0)
		return;
	if (n > 4)
		n = 1; // the game fell behind; do not replay a burst of retraces
	while (n--) {
		g_retrace_count++;
		g_det_time_calls = 0;
		// Audio DMA and trace numbering retain the native 59.94 Hz clock.
		if (g_retrace_count % g_retrace_multiplier == 0) {
			if (port_trace_on_retrace)
				port_trace_on_retrace(g_retrace_count / g_retrace_multiplier);
			if (port_audio_on_retrace)
				port_audio_on_retrace();
		}
		port_pad_autopress_field(game_field());
		if (g_pre)
			g_pre(g_retrace_count);
		if (g_next_fb) {
			g_cur_fb = g_next_fb;
		}
		OSWakeupThread(&g_retrace_queue);
		if (g_post)
			g_post(g_retrace_count);
	}
}
} // namespace

extern "C" void port_vi_init(void)
{
	g_retrace_multiplier = port_vi_retrace_multiplier();
	OSInitThreadQueue(&g_retrace_queue);
	shots_init();
	if (GXPC_SetIdleClock)
		GXPC_SetIdleClock(port_idle_seconds); // the overlay's frame breakdown
	port_irq_add_source(retrace_irq);
	// SMS_VI_FIELD_BASE=<n>: the retrace counter starts at n instead of 0, to
	// stand for the time retail spends before the game's first frame (IPL,
	// apploader, DOL load) and to pick the field parity of game frames.
	if (const char* b = getenv("SMS_VI_FIELD_BASE")) {
		g_field_base    = (u32)strtoul(b, NULL, 0);
		g_retrace_count = g_field_base * g_retrace_multiplier;
	}
	const char* d = getenv("SMS_VI_DETERMINISTIC");
	g_det         = d && *d && strcmp(d, "0") != 0;
	if (g_det) {
		port_log("[vi] deterministic retrace clock (SMS_VI_DETERMINISTIC)\n");
		return;
	}
	pthread_t th;
	pthread_create(&th, NULL, timer_thread, NULL);
	pthread_detach(th);
}

// Called by the scheduler when no thread can run. In deterministic mode the
// next retrace fires now; returns 1 if it did.
extern "C" int port_vi_idle_advance(void)
{
	if (!g_det)
		return 0;
	g_pending.fetch_add(1);
	g_det_polls = 0;
	return 1;
}

extern "C" int port_vi_deterministic(void) { return g_det; }

// Virtual OSTime in deterministic mode, relative to the boot time base.
extern "C" s64 port_vi_virtual_ticks(void)
{
	s64 step = (s64)(g_det_time_calls++) * 64;
	s64 ticks_per_retrace = kTicksPerField / g_retrace_multiplier;
	if (step >= ticks_per_retrace) {
		step = ticks_per_retrace - 1;
		// A thread spinning on the clock (TMarDirector::thpInit waits half a
		// second in an OSGetTick/OSYieldThread loop) would never let the game
		// idle: once the field's time is used up, the next retrace is due.
		if (g_pending.load() == 0)
			g_pending.fetch_add(1);
	}
	return (s64)g_retrace_count * kTicksPerField / g_retrace_multiplier + step;
}

extern "C" void VIInit(void) {}
extern "C" void VIConfigure(GXRenderModeObj* rm) {}
extern "C" void VIConfigurePan(u16, u16, u16, u16) {}
extern "C" void VIFlush(void) { shots_poll(game_field(), g_next_fb); }
extern "C" void VISetNextFrameBuffer(void* fb) { g_next_fb = fb; }
extern "C" void VISetNextRightFrameBuffer(void*) {}
extern "C" void VISetBlack(BOOL black) { g_black = black != 0; }
extern "C" void VISet3D(BOOL) {}
extern "C" u32 VIGetRetraceCount(void)
{
	// Deterministic mode: a busy-wait on the counter must still see time pass.
	if (g_det && ++g_det_polls >= 4096) {
		g_det_polls = 0;
		g_pending.fetch_add(1);
		port_irq_kick();
	}
	port_irq_check();
	return g_retrace_count;
}
extern "C" u32 VIGetNextField(void) { return 1; }
extern "C" u32 VIGetCurrentLine(void) { return 0; }
extern "C" u32 VIGetTvFormat(void) { return 0; } // VI_NTSC
extern "C" u32 VIGetDTVStatus(void) { return 0; }
extern "C" VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
	VIRetraceCallback old = g_pre;
	g_pre                 = cb;
	return old;
}
extern "C" VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
	VIRetraceCallback old = g_post;
	g_post                = cb;
	return old;
}
extern "C" void VIWaitForRetrace(void)
{
	BOOL lvl = OSDisableInterrupts();
	u32 cnt  = g_retrace_count;
	while (cnt == g_retrace_count)
		OSSleepThread(&g_retrace_queue);
	OSRestoreInterrupts(lvl);
}

// The end of JDrama::TVideo::waitForRetrace (framerate-37): waits for the
// retrace `due` this frame is shown at and returns the one the next frame is
// due at, `frames` later. Retail waits for the next retrace after each frame
// and counts the next frame from there, so a frame that ends just past its
// retrace costs a whole extra one: where the host's present waits for the
// display (vsync, macOS, the Steam Deck's compositor, at any refresh rate),
// frames at 60 or 120 fps kept missing and gameplay ran at half speed. Here
// frames keep to the schedule instead: a late frame starts the next at once,
// and one a whole frame behind is not presented (but one is at least every
// 0.05 s), so the game keeps time when the display takes fewer frames than it
// makes. That is only asked when the frames' own work fits in their time and
// presenting is slow (sms_gx), so a host too slow for the frame rate shows
// every frame. A frame more than 0.1 s late (a load) starts a new schedule.
// The deterministic clock keeps retail's wait.
extern "C" s32 port_vi_frame_wait(s32 due, u16 frames)
{
	const s32 max_behind = 6 * g_retrace_multiplier; // native fields: 0.1 s
	const s32 max_unshown = 3 * g_retrace_multiplier; // 0.05 s
	static s32 shown; // the retrace the last presented frame ended at
	static double ready, work; // when the last wait ended; seconds per frame, presenting aside
	if (g_det) {
		VIWaitForRetrace();
		return (s32)VIGetRetraceCount() + frames;
	}
	const double entry = seconds_now();
	if (ready > 0)
		work += (entry - ready - (GXPC_LastPresentSeconds ? GXPC_LastPresentSeconds() : 0) - work) * 0.1;
	while ((s32)(VIGetRetraceCount() - due) < 0)
		VIWaitForRetrace();
	const s32 now = (s32)VIGetRetraceCount();
	if (now - due > max_behind)
		due = now;
	const int speed = GXPC_GetSpeed ? GXPC_GetSpeed() : 1;
	const double period = frames * g_period_ns * 1e-9 / (speed > 0 ? speed : 1);
	const bool skip = now - due >= frames && now - shown < max_unshown && work < period;
	if (!skip)
		shown = now;
	if (GXPC_SkipNextPresent)
		GXPC_SkipNextPresent(skip);
	ready = seconds_now();
	return due + frames;
}

extern "C" void* port_vi_current_fb(void) { return g_cur_fb; }
