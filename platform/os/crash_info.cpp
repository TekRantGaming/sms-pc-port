// Crash report sections shared by every platform: what a faulting address
// points into, memory around the fault, the emulated threads, the disc files
// the game read last, and the build and system. Called from a signal handler
// or a vectored exception handler: no allocation, no stdio, no locks.
#include "crash.h"
#include "crash_line.h"
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <stdio.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach-o/dyld.h>
#else
#include <sys/uio.h>
#endif
#endif

namespace {
// Set by the runtime once MEM1 is mapped (the tests link this file without it).
uintptr_t g_mem1_base;
size_t g_mem1_size;
char g_build[512];
char g_system[512];
char g_executable[512];
double g_start_seconds;
PortCrashDescriber g_thread_describer;

enum { kFiles = 8, kFileLength = 120 };
char g_files[kFiles][kFileLength];
volatile unsigned g_next_file;

#ifdef _WIN32
HANDLE g_error_output = INVALID_HANDLE_VALUE;
#endif

double now_seconds()
{
#ifdef _WIN32
	return GetTickCount64() / 1000.0;
#else
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

void append(char* to, size_t size, const char* text)
{
	size_t at = strlen(to);
	while (*text && at + 1 < size)
		to[at++] = *text++;
	to[at] = 0;
}

void append_number(char* to, size_t size, unsigned long value)
{
	PortCrashLine line;
	line.number(value, 10);
	line.bytes[line.length < sizeof line.bytes ? line.length : sizeof line.bytes - 1] = 0;
	append(to, size, line.bytes);
}

bool in_mem1(uintptr_t address)
{
	return g_mem1_base && address >= g_mem1_base && address - g_mem1_base < g_mem1_size;
}
} // namespace

void port_crash_emit(PortCrashLine& line)
{
#ifdef _WIN32
	HANDLE output = g_error_output != INVALID_HANDLE_VALUE ? g_error_output : GetStdHandle(STD_ERROR_HANDLE);
	DWORD written;
	WriteFile(output, line.bytes, (DWORD)line.length, &written, NULL);
#else
	(void)!write(STDERR_FILENO, line.bytes, line.length);
#endif
	line.length = 0;
}

bool port_crash_read(void* to, uintptr_t from, size_t size)
{
#ifdef _WIN32
	SIZE_T copied = 0;
	return ReadProcessMemory(GetCurrentProcess(), (const void*)from, to, size, &copied) && copied == size;
#elif defined(__APPLE__)
	vm_size_t copied = 0;
	return vm_read_overwrite(mach_task_self(), (vm_address_t)from, size, (vm_address_t)to, &copied) == KERN_SUCCESS &&
	       copied == size;
#else
	// The kernel checks the source and fails with EFAULT instead of faulting.
	struct iovec local = {to, size}, remote = {(void*)from, size};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#endif
}

bool port_crash_module(uintptr_t address, uintptr_t* base, const char** name)
{
#ifdef _WIN32
	// The previous answer is kept: a backtrace stays in a few modules, and
	// each new one asks the loader.
	static uintptr_t cached_base;
	static char cached_name[MAX_PATH];
	MEMORY_BASIC_INFORMATION memory;
	if (!VirtualQuery((void*)address, &memory, sizeof memory) || memory.State != MEM_COMMIT)
		return false;
	uintptr_t found = (uintptr_t)memory.AllocationBase;
	if (found != cached_base) {
		cached_name[0] = 0;
		GetModuleFileNameA((HMODULE)found, cached_name, sizeof cached_name);
		cached_name[sizeof cached_name - 1] = 0;
		cached_base = found;
	}
	const char* path = cached_name;
#else
	Dl_info info;
	if (!dladdr((void*)address, &info) || !info.dli_fbase)
		return false;
	uintptr_t found = (uintptr_t)info.dli_fbase;
	const char* path = info.dli_fname ? info.dli_fname : "";
#endif
	const char* file = path;
	for (const char* c = path; *c; c++)
		if (*c == '\\' || *c == '/') file = c + 1;
	*base = found;
	*name = *file ? file : "<unknown>";
	return true;
}

void port_crash_prepare_info()
{
#ifdef _WIN32
	g_error_output = (HANDLE)_get_osfhandle(_fileno(stderr));
	if (g_error_output == INVALID_HANDLE_VALUE) g_error_output = GetStdHandle(STD_ERROR_HANDLE);
#endif
	if (g_build[0])
		return;
	g_start_seconds = now_seconds();
#ifdef __clang__
	append(g_build, sizeof g_build, "clang ");
#elif defined(__GNUC__)
	append(g_build, sizeof g_build, "gcc ");
#endif
#ifdef __VERSION__
	append(g_build, sizeof g_build, __VERSION__);
#endif
	append(g_build, sizeof g_build, ", ");
	append_number(g_build, sizeof g_build, sizeof(void*) * 8);
	append(g_build, sizeof g_build, "-bit ");
#if defined(__x86_64__) || defined(_M_X64)
	append(g_build, sizeof g_build, "x86-64");
#elif defined(__i386__) || defined(_M_IX86)
	append(g_build, sizeof g_build, "x86");
#elif defined(__aarch64__)
	append(g_build, sizeof g_build, "arm64");
#endif
#ifdef SMS_ECLIPSE
	append(g_build, sizeof g_build, ", Eclipse");
#endif
#ifdef NDEBUG
	append(g_build, sizeof g_build, ", NDEBUG");
#endif

#ifdef _WIN32
	typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW*);
	typedef const char*(CDECL * WineVersionFn)(void);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	OSVERSIONINFOW version = {};
	version.dwOSVersionInfoSize = sizeof version;
	GetVersionFn get_version = ntdll ? (GetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
	append(g_system, sizeof g_system, "Windows ");
	if (get_version && get_version(&version) == 0) {
		append_number(g_system, sizeof g_system, version.dwMajorVersion);
		append(g_system, sizeof g_system, ".");
		append_number(g_system, sizeof g_system, version.dwMinorVersion);
		append(g_system, sizeof g_system, " build ");
		append_number(g_system, sizeof g_system, version.dwBuildNumber);
	}
	WineVersionFn wine = ntdll ? (WineVersionFn)(void*)GetProcAddress(ntdll, "wine_get_version") : NULL;
	if (wine) {
		append(g_system, sizeof g_system, " (Wine ");
		append(g_system, sizeof g_system, wine());
		append(g_system, sizeof g_system, ")");
	}
	SYSTEM_INFO system;
	GetNativeSystemInfo(&system);
	append(g_system, sizeof g_system, ", ");
	append_number(g_system, sizeof g_system, system.dwNumberOfProcessors);
	append(g_system, sizeof g_system, " processors");
	GetModuleFileNameA(NULL, g_executable, sizeof g_executable);
	g_executable[sizeof g_executable - 1] = 0;
#else
	struct utsname name;
	if (uname(&name) == 0) {
		append(g_system, sizeof g_system, name.sysname);
		append(g_system, sizeof g_system, " ");
		append(g_system, sizeof g_system, name.release);
		append(g_system, sizeof g_system, " ");
		append(g_system, sizeof g_system, name.machine);
		append(g_system, sizeof g_system, " (");
		append(g_system, sizeof g_system, name.version);
		append(g_system, sizeof g_system, ")");
	}
	long processors = sysconf(_SC_NPROCESSORS_ONLN);
	if (processors > 0) {
		append(g_system, sizeof g_system, ", ");
		append_number(g_system, sizeof g_system, (unsigned long)processors);
		append(g_system, sizeof g_system, " processors");
	}
#ifdef __APPLE__
	uint32_t size = sizeof g_executable;
	if (_NSGetExecutablePath(g_executable, &size) != 0)
		g_executable[0] = 0;
#else
	ssize_t length = readlink("/proc/self/exe", g_executable, sizeof g_executable - 1);
	g_executable[length > 0 ? length : 0] = 0;
#endif
#endif
}

const char* port_crash_executable() { return g_executable; }

void port_crash_describe_address(const char* label, uintptr_t address)
{
	PortCrashLine line;
	line.text("[port] "); line.text(label); line.text(" "); line.hex(address); line.text(": ");
	uintptr_t base;
	const char* name;
	if (address < 0x10000) {
		line.text("in the null page (a null pointer plus "); line.hex(address); line.text(")");
	} else if (in_mem1(address)) {
		line.text("in emulated MEM1, GameCube address ");
		line.hex(0x80000000u + (address - g_mem1_base));
	} else if (address >= 0x80000000u && address < 0x90000000u && g_mem1_base == 0x80000000u) {
		// The game's pointers are u32 GameCube addresses.
		line.text("a GameCube address past the end of emulated MEM1 ("); line.number(g_mem1_size >> 20, 10);
		line.text(" MiB)");
	} else if (sizeof(void*) == 8 && (uint64_t)address >> 32 && in_mem1((uint32_t)address)) {
		line.text("upper 32 bits set on what would be a MEM1 address (a pointer stored in a u32, or garbage)");
	} else if (port_crash_module(address, &base, &name)) {
		line.text("in "); line.text(name); line.text(" at offset "); line.hex(address - base);
	} else {
		line.text("not in emulated MEM1 or a loaded module");
	}
	line.text("\n");
	port_crash_emit(line);
}

void port_crash_dump_memory(uintptr_t pc, uintptr_t sp)
{
	PortCrashLine line;
	unsigned char code[32];
	uintptr_t from = pc - 16;
	size_t count = sizeof code;
	if (!port_crash_read(code, from, count)) {
		from = pc, count = 16;
		if (!port_crash_read(code, from, count))
			count = 0;
	}
	line.text("[port] code at "); line.hex(from); line.text(":");
	for (size_t i = 0; i < count; i++) {
		line.text(from + i == pc ? " | " : " ");
		line.hex_digits(code[i], 2);
	}
	if (!count) line.text(" unreadable");
	line.text("\n");
	port_crash_emit(line);

	enum { kWords = 24, kPerLine = 4 };
	uintptr_t words[kWords];
	size_t readable = kWords;
	while (readable && !port_crash_read(words, sp, readable * sizeof(uintptr_t)))
		readable /= 2;
	if (!readable) {
		line.text("[port] stack at "); line.hex(sp); line.text(": unreadable\n");
		port_crash_emit(line);
		return;
	}
	for (size_t i = 0; i < readable; i += kPerLine) {
		line.text("[port] stack "); line.hex(sp + i * sizeof(uintptr_t)); line.text(":");
		for (size_t j = i; j < i + kPerLine && j < readable; j++) {
			line.text(" ");
			line.hex_digits(words[j], sizeof(uintptr_t) * 2);
		}
		line.text("\n");
		port_crash_emit(line);
	}
}

void port_crash_note_file(const char* path)
{
	if (!path)
		return;
	char* slot = g_files[g_next_file++ % kFiles];
	size_t i = 0;
	for (; path[i] && i + 1 < kFileLength; i++)
		slot[i] = path[i];
	slot[i] = 0;
}

void port_crash_set_mem1(uintptr_t base, size_t size)
{
	g_mem1_base = base;
	g_mem1_size = size;
}

void port_crash_set_thread_describer(PortCrashDescriber describer) { g_thread_describer = describer; }

void port_crash_describe_context()
{
	PortCrashLine line;
	unsigned next = g_next_file;
	line.text("[port] recent disc files (oldest first):");
	bool any = false;
	for (unsigned i = next < kFiles ? 0 : next - kFiles; i < next; i++) {
		const char* file = g_files[i % kFiles];
		if (!*file) continue;
		line.text(any ? ", " : " ");
		line.text(file);
		any = true;
	}
	if (!any) line.text(" none");
	line.text("\n");
	port_crash_emit(line);

	if (g_thread_describer)
		g_thread_describer();

	line.text("[port] build: "); line.text(g_build[0] ? g_build : "unknown"); line.text("\n");
	port_crash_emit(line);
	line.text("[port] system: "); line.text(g_system[0] ? g_system : "unknown"); line.text("\n");
	port_crash_emit(line);
	if (g_executable[0]) {
		line.text("[port] executable: "); line.text(g_executable); line.text("\n");
		port_crash_emit(line);
	}
	if (g_start_seconds > 0) {
		double running = now_seconds() - g_start_seconds;
		line.text("[port] running for "); line.number((uint64_t)running, 10); line.text(".");
		line.number((uint64_t)(running * 10) % 10, 10); line.text(" s\n");
		port_crash_emit(line);
	}
}
