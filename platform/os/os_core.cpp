// OS core: arena, clocks and time, reporting, OSAlloc heaps, caches (no-ops),
// stopwatches, RTC/reset/console queries.
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/os.h>
#include <dolphin/os/OSAlloc.h>
#include <dolphin/os/OSCache.h>
#include <dolphin/os/OSStopwatch.h>
#include <dolphin/os/OSRtc.h>
#include <dolphin/os/OSReset.h>
#include <dolphin/os/OSResetSW.h>
#include <dolphin/os/OSError.h>
#include <dolphin/os/OSMemory.h>
#include <time.h>
#include <execinfo.h>

extern "C" void port_os_threads_init(void);

static void* s_arena_lo;
static void* s_arena_hi;
static s64 s_time_base;      // OSTime at port start
static struct timespec s_t0; // host monotonic time at port start

extern "C" void port_os_init(void)
{
	__OSBusClock        = 162000000;
	__OSCoreClock       = 486000000;
	__OSPhysicalMemSize = port_mem1_size;
	__OSSimulatedMemSize = port_mem1_size;
	__OSTVMode          = 0; // NTSC
	// Leave the low 16 KiB as the OS globals page, as on hardware.
	s_arena_lo = port_mem1_base + 0x4000;
	s_arena_hi = port_mem1_base + port_mem1_size;
	clock_gettime(CLOCK_MONOTONIC, &s_t0);
	// OSTime counts from 2000-01-01 00:00:00 local time.
	time_t now = time(NULL);
	struct tm lt;
#ifdef _WIN32
	localtime_s(&lt, &now);
	s64 local = (s64)_mkgmtime(&lt) - 946684800LL;
#else
	localtime_r(&now, &lt);
	s64 local = (s64)now + lt.tm_gmtoff - 946684800LL;
#endif
	s_time_base = local * (s64)(__OSBusClock / 4);
	// Deterministic runs start at a fixed date (2002-07-19, the game's release).
	if (const char* d = getenv("SMS_VI_DETERMINISTIC"))
		if (*d && strcmp(d, "0") != 0)
			s_time_base = (s64)(1027036800LL - 946684800LL) * (s64)(__OSBusClock / 4);
	port_os_threads_init();
}

// --- Arena -------------------------------------------------------------------

extern "C" void* OSGetArenaLo(void) { return s_arena_lo; }
extern "C" void* OSGetArenaHi(void) { return s_arena_hi; }
extern "C" void OSSetArenaLo(void* p) { s_arena_lo = p; }
extern "C" void OSSetArenaHi(void* p) { s_arena_hi = p; }
extern "C" void* OSAllocFromArenaLo(u32 size, u32 align)
{
	uintptr_t p = ((uintptr_t)s_arena_lo + align - 1) & ~(uintptr_t)(align - 1);
	s_arena_lo  = (void*)(((p + size) + align - 1) & ~(uintptr_t)(align - 1));
	return (void*)p;
}
extern "C" void* OSAllocFromArenaHi(u32 size, u32 align)
{
	uintptr_t p = ((uintptr_t)s_arena_hi) & ~(uintptr_t)(align - 1);
	p           = (p - size) & ~(uintptr_t)(align - 1);
	s_arena_hi  = (void*)p;
	return (void*)p;
}
extern "C" u32 OSGetPhysicalMemSize(void) { return port_mem1_size; }
extern "C" u32 OSGetConsoleSimulatedMemSize(void) { return port_mem1_size; }
extern "C" unsigned long OSGetConsoleType(void) { return 0x10000006; } // retail HW2
extern "C" void OSInit(void) {}

// --- Time --------------------------------------------------------------------

extern "C" s64 port_time_ticks(void)
{
	if (port_vi_deterministic())
		return s_time_base + port_vi_virtual_ticks();
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	s64 ns = (s64)(t.tv_sec - s_t0.tv_sec) * 1000000000LL + (t.tv_nsec - s_t0.tv_nsec);
	// 40.5 MHz timer: ticks = ns * 81 / 2000
	return s_time_base + ns / 2000 * 81 + (ns % 2000) * 81 / 2000;
}

extern "C" OSTime OSGetTime(void)
{
	port_irq_check();
	return port_time_ticks();
}

extern "C" OSTick OSGetTick(void)
{
	port_irq_check();
	return (OSTick)port_time_ticks();
}

extern "C" void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
	s64 clock   = __OSBusClock / 4;
	s64 secs    = ticks / clock;
	s64 rem     = ticks % clock;
	if (rem < 0) {
		rem += clock;
		secs--;
	}
	time_t t = (time_t)(secs + 946684800LL);
	struct tm g;
#ifdef _WIN32
	gmtime_s(&g, &t);
#else
	gmtime_r(&t, &g); // ticks are already local time
#endif
	td->sec  = g.tm_sec;
	td->min  = g.tm_min;
	td->hour = g.tm_hour;
	td->mday = g.tm_mday;
	td->mon  = g.tm_mon;
	td->year = g.tm_year + 1900;
	td->wday = g.tm_wday;
	td->yday = g.tm_yday;
	td->msec = (int)(rem * 1000 / clock);
	td->usec = (int)(rem * 1000000 / clock % 1000);
}

// --- Reporting -----------------------------------------------------------------

extern "C" void OSReport(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("[OSReport] ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

extern "C" void OSPanic(const char* file, int line, const char* msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "[OSPanic] %s:%d: ", file, line);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	fputc('\n', stderr);
	void* bt[64];
	int n = backtrace(bt, 64);
	backtrace_symbols_fd(bt, n, 2);
	abort();
}

extern "C" OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) { return NULL; }
extern "C" void OSFillFPUContext(OSContext* context) {}
extern "C" void OSProtectRange(u32 channel, void* address, u32 nBytes, u32 control) {}

// --- Caches: coherent on the host ---------------------------------------------

// The GX layer caches decoded textures by address; a flush tells it the CPU
// rewrote memory (weak: absent without platform/gx).
extern "C" __attribute__((weak)) void GXPC_InvalidateRange(const void* p, u32 n);
static inline void gx_invalidate(void* p, u32 n)
{
	if (GXPC_InvalidateRange)
		GXPC_InvalidateRange(p, n);
}
extern "C" __attribute__((weak)) void GXPC_MemoryWritten(const void* p, u32 n);
static inline void gx_written(void* p, u32 n)
{
	if (GXPC_MemoryWritten)
		GXPC_MemoryWritten(p, n);
}
extern "C" void DCInvalidateRange(void*, u32) {}
extern "C" void DCFlushRange(void* p, u32 n)
{
	gx_invalidate(p, n);
	port_irq_check();
}
extern "C" void DCStoreRange(void* p, u32 n) { gx_invalidate(p, n); }
extern "C" void DCFlushRangeNoSync(void* p, u32 n) { gx_invalidate(p, n); }
extern "C" void DCStoreRangeNoSync(void* p, u32 n) { gx_invalidate(p, n); }
extern "C" void DCZeroRange(void* addr, u32 n)
{
	memset(addr, 0, n);
	gx_written(addr, n);
}
extern "C" void DCTouchRange(void*, u32) {}
extern "C" void ICInvalidateRange(void*, u32) {}
extern "C" void LCEnable(void) {}
extern "C" void LCDisable(void) {}
extern "C" void PPCSync(void) {}
extern "C" u32 PPCMfmsr(void) { return 0x8000; }

// --- OSAlloc: first-fit heaps inside caller-supplied ranges -------------------

namespace {
struct Cell {
	Cell* next;
	u32 size; // including header
	u32 used;
};
struct Heap {
	u8* start;
	u8* end;
	Cell* cells;
	bool live;
};
Heap* s_heaps;
int s_max_heaps;
int s_cur_heap = -1;
const u32 kHdr = 32;
} // namespace

extern "C" void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
	s_heaps     = (Heap*)arenaStart;
	s_max_heaps = maxHeaps;
	memset(s_heaps, 0, sizeof(Heap) * maxHeaps);
	uintptr_t p = (uintptr_t)arenaStart + sizeof(Heap) * maxHeaps;
	return (void*)((p + 31) & ~(uintptr_t)31);
}

extern "C" int OSCreateHeap(void* start, void* end)
{
	for (int i = 0; i < s_max_heaps; i++) {
		if (s_heaps[i].live)
			continue;
		Heap& h = s_heaps[i];
		h.start = (u8*)(((uintptr_t)start + 31) & ~(uintptr_t)31);
		h.end   = (u8*)((uintptr_t)end & ~(uintptr_t)31);
		h.cells = (Cell*)h.start;
		h.cells->next = NULL;
		h.cells->size = (u32)(h.end - h.start);
		h.cells->used = 0;
		h.live        = true;
		return i;
	}
	return -1;
}

extern "C" void OSDestroyHeap(int heap)
{
	if (heap >= 0 && heap < s_max_heaps)
		s_heaps[heap].live = false;
}

extern "C" int OSSetCurrentHeap(int heap)
{
	int old    = s_cur_heap;
	s_cur_heap = heap;
	return old;
}

extern "C" void* OSAllocFromHeap(int heap, unsigned long size)
{
	if (heap < 0 || heap >= s_max_heaps || !s_heaps[heap].live)
		return NULL;
	u32 need = (u32)((size + kHdr + 31) & ~31ul);
	for (Cell* c = s_heaps[heap].cells; c; c = c->next) {
		if (c->used || c->size < need)
			continue;
		if (c->size >= need + 64) {
			Cell* n = (Cell*)((u8*)c + need);
			n->next = c->next;
			n->size = c->size - need;
			n->used = 0;
			c->next = n;
			c->size = need;
		}
		c->used = 1;
		return (u8*)c + kHdr;
	}
	return NULL;
}

extern "C" void OSFreeToHeap(int heap, void* ptr)
{
	if (!ptr || heap < 0 || heap >= s_max_heaps)
		return;
	Cell* c = (Cell*)((u8*)ptr - kHdr);
	c->used = 0;
	for (Cell* a = s_heaps[heap].cells; a; a = a->next)
		while (!a->used && a->next && !a->next->used && (u8*)a + a->size == (u8*)a->next) {
			a->size += a->next->size;
			a->next = a->next->next;
		}
}

extern "C" long OSCheckHeap(int heap)
{
	if (heap < 0 || heap >= s_max_heaps || !s_heaps[heap].live)
		return -1;
	long free = 0;
	for (Cell* c = s_heaps[heap].cells; c; c = c->next)
		if (!c->used)
			free += c->size - kHdr;
	return free;
}

extern "C" unsigned long OSReferentSize(void* ptr) { return ((Cell*)((u8*)ptr - kHdr))->size - kHdr; }
extern "C" void OSDumpHeap(int heap) { port_log("[os] OSDumpHeap(%d): %ld free\n", heap, OSCheckHeap(heap)); }

// --- Stopwatches -----------------------------------------------------------------

extern "C" void OSInitStopwatch(OSStopwatch* sw, char* name)
{
	sw->name = name;
	OSResetStopwatch(sw);
}
extern "C" void OSStartStopwatch(OSStopwatch* sw)
{
	sw->running = 1;
	sw->last    = OSGetTime();
}
extern "C" void OSStopStopwatch(OSStopwatch* sw)
{
	if (!sw->running)
		return;
	long long d = OSGetTime() - sw->last;
	sw->total += d;
	sw->running = 0;
	sw->hits++;
	if (d > sw->max)
		sw->max = d;
	if (d < sw->min)
		sw->min = d;
}
extern "C" long long OSCheckStopwatch(OSStopwatch* sw)
{
	long long t = sw->total;
	if (sw->running)
		t += OSGetTime() - sw->last;
	return t;
}
extern "C" void OSResetStopwatch(OSStopwatch* sw)
{
	sw->total   = 0;
	sw->hits    = 0;
	sw->min     = 0x7FFFFFFFFFFFFFFFLL;
	sw->max     = 0;
	sw->running = 0;
}
extern "C" void OSDumpStopwatch(OSStopwatch* sw)
{
	port_log("[os] stopwatch %s: %llu hits, total %lld\n", sw->name ? sw->name : "?", (unsigned long long)sw->hits, sw->total);
}

// --- RTC / reset / modes -----------------------------------------------------------

static u32 s_sound_mode = 1, s_progressive = 0;
extern "C" u32 OSGetSoundMode(void) { return s_sound_mode; }
extern "C" void OSSetSoundMode(u32 mode) { s_sound_mode = mode; }
extern "C" u32 OSGetProgressiveMode(void) { return s_progressive; }
extern "C" void OSSetProgressiveMode(u32 mode) { s_progressive = mode; }
extern "C" unsigned long OSGetResetCode(void) { return 0; }
extern "C" BOOL OSGetResetSwitchState(void) { return FALSE; }
extern "C" BOOL OSGetResetButtonState(void) { return FALSE; }
extern "C" void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
	port_log("[os] OSResetSystem(%d, 0x%x, %d): exiting\n", reset, resetCode, forceMenu);
	exit(0);
}
