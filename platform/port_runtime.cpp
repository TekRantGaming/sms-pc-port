// Port runtime: logging, stub accounting, and the boot sequence that stands in
// for the GameCube IPL/__start/OSInit before SMS_main runs.
#include "sms_mod/modhooks.h"
#include "port_compat.h"
#include "port_platform.h"
#include "port_framerate.h"
#include "port_stub.h"
#include "port_os.h"
#include <dolphin/os.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <execinfo.h>
#include "port_host.h"
#include "disc/gcdisc.h"
#include "os/crash.h"
#include "os/crash_line.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#ifndef __APPLE__
#include <ucontext.h>
#endif
#ifdef __APPLE__
#include <sys/ucontext.h>
#endif
#endif

#ifdef _WIN32
// Laptops with two GPUs (NVIDIA Optimus, AMD PowerXpress) run a program on the
// integrated one unless the executable exports these: without them the game
// renders on the slower GPU even when a dedicated one is there.
extern "C" {
__attribute__((dllexport)) unsigned long NvOptimusEnablement = 1;
__attribute__((dllexport)) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// Game source named by a bare argument or SMS_DISC_ROOT (a disc image or an
// extracted files/ folder); SMS_DISC_IMAGE overrides it. With none of them,
// the image bundled into the executable (tools/bundle_disc.py) is used, else
// on Windows the working directory as an extracted folder.
const char* port_disc_root =
#ifdef _WIN32
    ".";
#else
    NULL;
#endif
// Set when the command line or SMS_DISC_ROOT named the game source; otherwise
// a disc image bundled into the executable wins over the default above.
int port_disc_explicit = 0;
// SMS_SKIP_MOVIES=1 reports every THP movie as finished at once (patch 0016).
extern "C" int port_skip_movies;
int port_skip_movies = 0;
// SMS_HEAT_HAZE=0 turns off the heat-wave shimmer (patch zzz-heat-haze-01).
extern "C" int port_heat_haze;
int port_heat_haze = 1;
// SMS_WIDESCREEN: the displayed width over the GameCube's 4:3 (1 when off);
// the game camera (widescreen-01 patch) and sms_gx widen by it.
extern "C" float port_widescreen;
float port_widescreen = 1.0f;
extern "C" __attribute__((weak)) void GXPC_SetWidescreen(float widthOver43);
// SMS_FRAME_RATE: 30 (the game's own), 60 (default), or 120, for gameplay (the Application
// patch framerate-01 reads it; logos, menus and movies stay at 30).
extern "C" int port_frame_rate;
int port_frame_rate = 60;
extern "C" int port_active_frame_rate;
int port_active_frame_rate = 30;
extern "C" int port_high_fps_active;
int port_high_fps_active = 0;

// "16:9", "21:9", "16:10", "on" (16:9), "off"/"0", or a ratio such as 1.85.
static float parse_widescreen(const char* v)
{
	if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "off"))
		return 1.0f;
	float aspect = 16.0f / 9.0f;
	float a = 0, b = 0;
	if (sscanf(v, "%f:%f", &a, &b) == 2 && a > 0 && b > 0)
		aspect = a / b;
	else if (strcmp(v, "1") && strcmp(v, "on") && atof(v) > 0)
		aspect = (float)atof(v);
	float f = aspect / (4.0f / 3.0f);
	if (f < 1.0f)
		f = 1.0f;
	if (f > 3.0f)
		f = 3.0f;
	return f;
}

extern "C" void port_log(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fflush(stderr);
}

static port_stub_rec* s_stub_list;
static unsigned s_stub_distinct;
extern "C" void port_stub_hit(port_stub_rec* rec)
{
	if (rec->count++ == 0) {
		rec->next   = s_stub_list;
		s_stub_list = rec;
		++s_stub_distinct;
		if (!getenv("SMS_QUIET_STUBS"))
			port_log("[stub] %s (first call; %u distinct stubs hit)\n", rec->name, s_stub_distinct);
	}
}

extern "C" void port_stub_report(void)
{
	port_log("[stub] summary: %u distinct SDK stubs were called\n", s_stub_distinct);
	for (port_stub_rec* r = s_stub_list; r; r = r->next)
		port_log("[stub]   %-32s %lu\n", r->name, r->count);
}

// Symbolise the backtrace with addr2line so a crash report names functions
// and source lines without the exact binary at hand (not async-signal-safe;
// acceptable on the way down).
#ifndef _WIN32
static void crash_symbolise(void** bt, int n)
{
	char exe[512];
	if (!gcdisc_self_path(exe, sizeof exe))
		return;
	static char addrs[64][24];
	char* argv[64 + 8];
	int argc = 0;
	argv[argc++] = (char*)"addr2line";
	argv[argc++] = (char*)"-f";
	argv[argc++] = (char*)"-C";
	argv[argc++] = (char*)"-p";
	argv[argc++] = (char*)"-e";
	argv[argc++] = exe;
	for (int i = 0; i < n && argc < 64 + 7; i++) {
		Dl_info info;
		if (!dladdr(bt[i], &info) || !info.dli_fname || !info.dli_fbase)
			continue;
		char self[512];
		if (!realpath(info.dli_fname, self) || strcmp(self, exe) != 0)
			continue;
		// return addresses point after the call; step back into it
		snprintf(addrs[i], sizeof addrs[i], "0x%lx",
		         (unsigned long)((char*)bt[i] - (char*)info.dli_fbase - 1));
		argv[argc++] = addrs[i];
	}
	argv[argc] = nullptr;
	if (argc == 6)
		return;
	port_log("[port] backtrace:\n");
	pid_t pid = fork();
	if (pid == 0) {
		dup2(2, 1);
		execvp("addr2line", argv);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, nullptr, 0);
}
#endif

#ifdef _WIN32
static void crash_handler(int sig)
{
	port_log("\n[port] fatal signal %d\n", sig);
	void* bt[64];
	int n = backtrace(bt, 64);
	backtrace_symbols_fd(bt, n, 2);
	port_stub_report();
	signal(sig, SIG_DFL);
	raise(sig);
}
#else
static void crash_handler(int sig, siginfo_t* si, void* uc)
{
	// Write the actual signal, fault address and instruction address first,
	// without stdio locks or allocation. Symbolisation below is best effort.
	PortCrashLine line;
	line.text("\n[port] fatal signal ");
	const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGFPE ? "SIGFPE"
	                 : sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT" : "unknown";
	line.text(name); line.text(" ("); line.number(sig, 10); line.text(")");
	// For user-raised signals si_addr aliases the sender's PID, not a fault.
	if (si && si->si_code > 0 && sig != SIGABRT) {
		line.text(" at address "); line.hex((uintptr_t)si->si_addr);
	}
	if (si) {
		line.text(" si_code ");
		if (si->si_code < 0) line.text("-");
		line.number(si->si_code < 0 ? -(int64_t)si->si_code : si->si_code, 10);
	}
	uintptr_t pc = 0;
	if (uc) {
#if defined(__APPLE__) && defined(__x86_64__)
		pc = ((ucontext_t*)uc)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
		pc = ((ucontext_t*)uc)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__x86_64__)
		pc = ((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__i386__)
		pc = ((ucontext_t*)uc)->uc_mcontext.gregs[REG_EIP];
#elif defined(__linux__) && defined(__aarch64__)
		pc = ((ucontext_t*)uc)->uc_mcontext.pc;
#endif
	}
	line.text(" pc "); line.hex(pc); line.text("\n");
	(void)write(STDERR_FILENO, line.bytes, line.length);
	Dl_info fault_module;
	if (pc && dladdr((void*)pc, &fault_module) && fault_module.dli_fbase)
		port_log("[port] exception module %s base %p offset 0x%llx\n",
		         fault_module.dli_fname ? fault_module.dli_fname : "<unknown>", fault_module.dli_fbase,
		         (unsigned long long)(pc - (uintptr_t)fault_module.dli_fbase));
#if defined(__APPLE__) && defined(__x86_64__)
	if (uc) {
		// No gdb on macOS and lldb needs developer-mode approval: print the
		// faulting registers so a bad pointer can be traced from the log.
		const auto& r = ((ucontext_t*)uc)->uc_mcontext->__ss;
		port_log("[port] rip=%llx rsp=%llx rbp=%llx\n"
		         "[port] rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx\n"
		         "[port] r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
		         r.__rip, r.__rsp, r.__rbp, r.__rax, r.__rbx, r.__rcx, r.__rdx, r.__rsi, r.__rdi,
		         r.__r8, r.__r9, r.__r10, r.__r11, r.__r12, r.__r13, r.__r14, r.__r15);
	}
#endif
	void* bt[64];
	int n = backtrace(bt, 64);
	backtrace_symbols_fd(bt, n, 2);
	crash_symbolise(bt, n);
	port_stub_report();
#ifdef __APPLE__
	// Under Rosetta, a translated process that dies from a re-raised fatal
	// signal hangs in the kernel's exit path (state UE, unkillable). Exit
	// with the shell's 128+signal status instead.
	_exit(128 + sig);
#else
	signal(sig, SIG_DFL);
	raise(sig);
#endif
}
#endif

void port_crash_thread_init()
{
#ifndef _WIN32
	// A separate stack allows the first diagnostic to survive stack exhaustion.
	alignas(16) static thread_local unsigned char crash_stack[64 * 1024];
	stack_t stack = {};
	stack.ss_sp = crash_stack;
	stack.ss_size = sizeof crash_stack;
	if (sigaltstack(&stack, nullptr) != 0)
		port_log("[port] cannot install alternate crash stack\n");
#endif
}

void port_install_crash_handlers()
{
#ifdef _WIN32
	port_install_windows_exception_logger();
	// Native hardware exceptions keep their Windows status. Translating them
	// through a CRT signal handler would replace it with a generic abort code.
	signal(SIGABRT, crash_handler);
#else
	struct sigaction sa = {};
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
		if (sigaction(sig, &sa, nullptr) != 0) port_log("[port] cannot install crash handler for signal %d\n", sig);
#endif
	port_crash_thread_init();
}

// Emulated MEM1: the game's arena lives at the GameCube's own cached
// addresses (0x80000000..), so OSPhysicalToCached/OSCachedToPhysical and the
// few raw low-memory reads (e.g. *(OSModuleInfo**)0x800030C8) work unchanged.
u8* port_mem1_base;
u32 port_mem1_size;

#ifndef _WIN32
// mmap exactly at `want` without replacing an existing mapping. Linux has
// MAP_FIXED_NOREPLACE; Darwin's MAP_FIXED silently replaces whatever is there
// (dylibs, graphics driver memory: the low 4 GiB is shared with the system
// once PAGEZERO is shrunk), so pass `want` as a hint and reject any other
// placement.
static void* map_exact(void* want, size_t size)
{
#ifdef MAP_FIXED_NOREPLACE
	void* p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#else
	void* p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	if (p == want)
		return p;
	if (p != MAP_FAILED)
		munmap(p, size);
	return MAP_FAILED;
}
#endif

static void map_mem1()
{
	// 64-bit hosts: objects holding pointers are larger, and the fixed-size
	// heaps grow with them (PORT_HEAP64), so give the game more MEM1.
	u32 mb = sizeof(void*) == 8 ? 64 : 24;
	if (const char* e = getenv("SMS_MEM_MB"))
		mb = (u32)atoi(e);
	port_mem1_size = mb << 20;
	void* want = (void*)(uintptr_t)0x80000000u;
#ifdef _WIN32
	void* p = VirtualAlloc(want, port_mem1_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (p != want) {
		port_log("[port] cannot map MEM1 at 0x80000000 (got %p)\n", p);
		if (p)
			VirtualFree(p, 0, MEM_RELEASE);
		exit(1);
	}
#else
	void* p = map_exact(want, port_mem1_size);
	if (p != want) {
		port_log("[port] cannot map MEM1 at 0x80000000; falling back to a heap block\n");
		p = mmap(NULL, port_mem1_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			port_log("[port] out of memory for MEM1\n");
			exit(1);
		}
	}
#endif
	port_mem1_base = (u8*)p;
	port_log("[port] MEM1: %u MiB at %p\n", mb, p);
}

// A PTR32 field (4-byte pointer slot in a struct laid over file data) was
// given an address above 4 GiB: something the game can reach was allocated
// high. Fatal, since the pointer would be silently truncated.
extern "C" void port_ptr32_trap(const void* p)
{
	port_log("[port] PTR32: address %p does not fit a 32-bit slot\n", p);
	abort();
}

void* port_low_alloc(unsigned long size)
{
#if UINTPTR_MAX <= 0xFFFFFFFFu
	return malloc(size);
#elif defined(_WIN32)
	// The first call (the boot thread's stack, before any window exists)
	// reserves a pool below 2 GiB that later stacks are committed from: a large
	// internal resolution, MSAA and texture packs make the GPU driver claim
	// much of the low address space once rendering starts.
	static char* pool;
	static size_t poolUsed;
	const size_t kPool = 128ul << 20;
	if (!pool)
		for (uintptr_t at = 0x10000000u; at + kPool <= 0x80000000u && !pool; at += 0x100000u)
			pool = (char*)VirtualAlloc((void*)at, kPool, MEM_RESERVE, PAGE_NOACCESS);
	const size_t grain = (size + 0xFFFFu) & ~(size_t)0xFFFFu;
	if (pool && poolUsed + grain <= kPool) {
		void* p = VirtualAlloc(pool + poolUsed, size, MEM_COMMIT, PAGE_READWRITE);
		if (p) {
			poolUsed += grain;
			return p;
		}
	}
	// Walk hint addresses from 256 MiB up to 2 GiB (64 KiB allocation grain).
	for (uintptr_t at = 0x10000000u; at + size <= 0x80000000u; at += 0x10000u) {
		void* p = VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p)
			return p;
	}
	return NULL;
#else
#ifdef MAP_32BIT
	void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (p != MAP_FAILED)
		return p;
#endif
	static uintptr_t next = 0x40000000u;
	for (int tries = 0; tries < 4096 && next + size <= 0x80000000u; tries++) {
		void* want = (void*)next;
		next += (size + 0xFFFFu) & ~(uintptr_t)0xFFFFu;
		void* q = map_exact(want, size);
		if (q == want)
			return q;
	}
	return NULL;
#endif
}

#ifdef _WIN64
namespace {
struct StackRun {
	void* jb[5]; // __builtin_setjmp buffer, on the thread's own stack
	void* base;  // the thread's own stack bounds (NT_TIB)
	void* limit;
	StackRun* outer;
};
thread_local StackRun* t_run;

// Windows x64 ABI: fn's first argument in rcx, 32 bytes of shadow space, rsp
// 16-byte aligned at the call. rbx (callee-saved, so fn keeps it) holds the
// thread's own rsp.
__attribute__((noinline)) void* call_on(void* top, void* (*fn)(void*), void* arg)
{
	void* ret;
	__asm__ volatile(
		"mov %%rsp, %%rbx\n\t"
		"mov %1, %%rsp\n\t"
		"sub $32, %%rsp\n\t"
		"mov %3, %%rcx\n\t"
		"call *%2\n\t"
		"mov %%rbx, %%rsp"
		: "=a"(ret)
		: "r"(top), "r"(fn), "r"(arg)
		: "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3",
		  "xmm4", "xmm5", "memory", "cc");
	return ret;
}
} // namespace

extern "C" void* port_run_on_stack(void* stack, size_t size, void* (*fn)(void*), void* arg)
{
	// Exception dispatch and stack walks check frames against the TEB's
	// stack bounds, so they follow the switch.
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	StackRun run;
	run.base = tib->StackBase;
	run.limit = tib->StackLimit;
	run.outer = t_run;
	void* volatile ret = NULL;
	if (__builtin_setjmp(run.jb) == 0) {
		t_run = &run;
		void* top = (void*)(((uintptr_t)stack + size) & ~(uintptr_t)15);
		tib->StackBase = top;
		tib->StackLimit = stack;
		ret = call_on(top, fn, arg);
	}
	tib->StackBase = run.base;
	tib->StackLimit = run.limit;
	t_run = run.outer;
	return ret;
}

extern "C" void port_leave_stack(void)
{
	if (t_run)
		__builtin_longjmp(t_run->jb, 1);
	pthread_exit(NULL);
}
#endif

// Hardware register window. The only direct access left in game code is the
// GX write-gather pipe (GXWGFifo at 0xCC008000, written by the inline GXVert.h
// vertex/command writers). Until the GX layer redirects those writes, map the
// window as a write sink so they are harmless.
static void map_hw_sink()
{
	void* want = (void*)(uintptr_t)0xCC000000u;
#ifdef _WIN32
	void* p = VirtualAlloc(want, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
	void* p = map_exact(want, 0x10000);
#endif
	if (p != want)
		port_log("[port] cannot map the hardware register sink at 0xCC000000 (%p)\n", p);
	else
		port_log("[port] GX WG pipe 0xCC008000 is a write sink\n");
}

// Window/headless switches belong to the GX layer (weak: absent without it).
extern "C" __attribute__((weak)) int GXPC_ParseArgs(int* argc, char** argv);
extern "C" __attribute__((weak)) void GXPC_SetHeadless(int headless);

// The 32-bit NVIDIA GLX library must match the kernel module's version
// exactly, or context creation fails (X_GLXCreateContext BadValue). If this
// host's i386 NVIDIA userspace does not match, use Mesa (llvmpipe) for the
// window instead, unless the user chose a GLX vendor.
static void pick_glx_vendor()
{
#ifndef _WIN32
	if (sizeof(void*) != 4 || getenv("__GLX_VENDOR_LIBRARY_NAME"))
		return;
	FILE* f = fopen("/proc/driver/nvidia/version", "r");
	if (!f)
		return;
	char line[256] = { 0 };
	fgets(line, sizeof line, f);
	fclose(f);
	const char* k = strstr(line, "Kernel Module");
	char ver[64]  = { 0 };
	if (!k || sscanf(k, "Kernel Module %63s", ver) != 1)
		return;
	char lib[256];
	snprintf(lib, sizeof lib, "/usr/lib/i386-linux-gnu/libGLX_nvidia.so.%s", ver);
	if (access(lib, R_OK) == 0)
		return;
	setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
	port_log("[port] no 32-bit NVIDIA GLX for kernel module %s; using Mesa for the window\n", ver);
#endif
}

// settings.txt: one option per line, `name = value`, read at start; an
// environment variable that is already set wins. The names in kSettings stand
// for the environment variables beside them (on/off become 1/0); any SMS_*
// variable can be given by its own name too. Found in the working directory,
// or two levels up when started from build/<os>-<arch>/, or at SMS_SETTINGS.
static const struct {
	const char* name;
	const char* env;
} kSettings[] = {
	{ "texture_packs", "SMS_TEXTURE_PACKS" }, // on (mods/textures), off, or folders
	{ "texture_pack_mb", "SMS_TEXTURE_PACK_MB" },
	{ "texture_pack_preload", "SMS_TEXTURE_PACK_PRELOAD" },
	{ "texture_pack_pending_mb", "SMS_TEXTURE_PACK_PENDING_MB" },
	{ "hd_cutscenes", "SMS_HD_CUTSCENES" }, // follows HD textures; 0 disables
	{ "widescreen", "SMS_WIDESCREEN" },
	{ "widescreen_hud", "SMS_WIDESCREEN_HUD" }, // centre or edges
	{ "frame_rate", "SMS_FRAME_RATE" },         // 30, 60 (default), or 120
	{ "mod", "SMS_MOD" },
	{ "resolution", "SMS_GX_SCALE" },
	{ "anisotropic", "SMS_ANISO" }, // 0, 2, 4, 8 or 16
	{ "window_scale", "SMS_WINDOW_SCALE" },
	{ "vsync", "SMS_VSYNC" },                     // on, off or adaptive
	{ "fullscreen", "SMS_FULLSCREEN" },           // off, on (desktop) or exclusive
	{ "fullscreen_mode", "SMS_FULLSCREEN_MODE" }, // WxH@Hz, for exclusive
	{ "display", "SMS_DISPLAY" },                 // monitor, 0 = primary
	{ "msaa", "SMS_MSAA" },                     // 0, 2, 4 or 8
	{ "fxaa", "SMS_FXAA" },
	{ "sharpen", "SMS_SHARPEN" },               // 0..100
	{ "brightness", "SMS_GAMMA" },              // 1.0 = unchanged
	{ "aspect", "SMS_ASPECT" },                 // keep, stretch or integer
	{ "present_filter", "SMS_PRESENT_FILTER" }, // bilinear, sharp or nearest
	{ "skip_movies", "SMS_SKIP_MOVIES" },
	{ "heat_haze", "SMS_HEAT_HAZE" }, // the heat-wave shimmer, on by default
	{ "audio", "SMS_AUDIO" },
	{ "volume", "SMS_VOLUME" }, // master volume, 0 to 100
	{ "soft_trigger", "SMS_SOFT_TRIGGER" }, // L_SOFT / R_SOFT press depth, percent
	{ "overlay", "SMS_OVERLAY" },
	{ "save_dir", "SMS_SAVE_DIR" },
	{ "disc_image", "SMS_DISC_IMAGE" },
	{ "camera_invert_x", "SMS_CAMERA_INVERT_X" },
	{ "camera_invert_y", "SMS_CAMERA_INVERT_Y" },
	{ "camera_speed", "SMS_CAMERA_SPEED" },           // percent, 100 = the game's own
	{ "free_camera", "SMS_FREE_CAMERA" },             // no automatic swing back
	{ "mouse_camera", "SMS_MOUSE_CAMERA" },           // mouse look
	{ "mouse_sensitivity", "SMS_MOUSE_SENSITIVITY" }, // percent
};

static void load_settings()
{
	const char* path = getenv("SMS_SETTINGS");
	FILE* f          = path ? fopen(path, "r") : NULL;
	if (!path) {
		path = "settings.txt";
		f    = fopen(path, "r");
		if (!f) {
			path = "../../settings.txt"; // running from build/<os>-<arch>/
			f    = fopen(path, "r");
		}
	}
	if (!f)
		return;
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof line, f)) {
		char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\r' || !*p)
			continue;
		char* eq = strchr(p, '=');
		if (!eq)
			continue;
		char* ke = eq;
		while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t'))
			ke--;
		*ke     = 0;
		char* v = eq + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		char* ve = v + strlen(v);
		while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t'))
			ve--;
		*ve = 0;
		if (char* c = strstr(v, " #")) { // trailing comment
			*c = 0;
			while (c > v && (c[-1] == ' ' || c[-1] == '\t'))
				*--c = 0;
		}
		const char* env = strncmp(p, "SMS_", 4) == 0 ? p : NULL;
		for (size_t i = 0; !env && i < sizeof kSettings / sizeof kSettings[0]; i++)
			if (strcmp(p, kSettings[i].name) == 0)
				env = kSettings[i].env;
		if (!env) {
			port_log("[port] %s: unknown setting \"%s\"\n", path, p);
			continue;
		}
		const char* val = v;
		if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true"))
			val = "1";
		else if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false"))
			val = "0";
		if (!strcmp(env, "SMS_TEXTURE_PACKS") && !strcmp(val, "1"))
			continue; // on: the default folder
		if (!*val || getenv(env))
			continue;
		port_setenv(env, val, 0);
		n++;
	}
	fclose(f);
	if (n)
		port_log("[port] %d settings from %s\n", n, path);
}

extern "C" void port_init(int argc, char** argv)
{
	load_settings();
	pick_glx_vendor();
	if (const char* m = getenv("SMS_SKIP_MOVIES"))
		port_skip_movies = *m && strcmp(m, "0") != 0;
	if (const char* h = getenv("SMS_HEAT_HAZE"))
		port_heat_haze = !*h || strcmp(h, "0") != 0;
	if (!port_heat_haze)
		port_log("[port] heat-wave shimmer off\n");
	port_widescreen = parse_widescreen(getenv("SMS_WIDESCREEN"));
	if (GXPC_SetWidescreen)
		GXPC_SetWidescreen(port_widescreen);
	if (port_widescreen > 1.0f)
		port_log("[port] widescreen: %.3f times the 4:3 width\n", port_widescreen);
	const char* frame_rate = getenv("SMS_FRAME_RATE");
	port_frame_rate = port_parse_frame_rate(frame_rate);
	if (frame_rate && strcmp(frame_rate, "30") && strcmp(frame_rate, "60") && strcmp(frame_rate, "120"))
		port_log("[port] unsupported frame rate '%s'; using 60\n", frame_rate);
	port_log("[port] frame rate: %d during gameplay\n", port_frame_rate);
	sms_mod_activate();
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--headless") == 0) {
			port_setenv("SMS_HEADLESS", "1", 1);
			if (GXPC_SetHeadless)
				GXPC_SetHeadless(1);
		}
	if (GXPC_ParseArgs)
		GXPC_ParseArgs(&argc, argv);
	if (const char* d = getenv("SMS_DISC_ROOT")) {
		port_disc_root     = d;
		port_disc_explicit = 1;
	}
	for (int i = 1; i < argc; i++)
		if (argv[i][0] != '-') {
			port_disc_root     = argv[i];
			port_disc_explicit = 1;
		}
	atexit(port_stub_report);
	map_mem1();
	if (sizeof(void*) == 4)
		map_hw_sink();
	port_os_init();
	port_dvd_init();
	port_window_icon_init();
	port_noaudio_init();
	port_vi_init();
	port_log("[port] platform ready\n");
}
