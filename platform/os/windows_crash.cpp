#ifdef _WIN32
#include "crash.h"
#include "crash_line.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
volatile LONG reporting;
// Drivers and system DLLs may raise and handle exceptions of their own; cap
// the reports so they cannot flood the log before a real crash. C++ throws
// have their own budget and never use up the one for faults.
volatile LONG fault_reports = 16;
volatile LONG throw_reports = 4;
// Running addr2line takes a moment: only the first faults get it.
volatile LONG symbolised_reports = 2;
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;

enum { kMaxFrames = 48 };
uintptr_t frames[kMaxFrames];
int frame_count;

// addr2line runs from a thread made at start-up: creating a process needs the
// heap and the loader, which the crashed thread may hold. The report waits a
// bounded time for it and goes on.
HANDLE symbolise_request, symbolise_done, inheritable_error;
char executable[MAX_PATH];
char addr2line[MAX_PATH] = "addr2line.exe";
char command[4096];
uintptr_t exe_base, exe_preferred_base;

DWORD WINAPI symboliser(void*)
{
	for (;;) {
		WaitForSingleObject(symbolise_request, INFINITE);
		STARTUPINFOA startup = {};
		startup.cb = sizeof startup;
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = NULL;
		startup.hStdOutput = startup.hStdError = inheritable_error;
		PROCESS_INFORMATION process;
		if (CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
			WaitForSingleObject(process.hProcess, 8000);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		} else {
			PortCrashLine line;
			line.text("[port] cannot run "); line.text(addr2line); line.text(" (error ");
			line.number(GetLastError(), 10); line.text("): frames left unsymbolised\n");
			port_crash_emit(line);
		}
		SetEvent(symbolise_done);
	}
}

// The image base the executable was linked at, which addr2line expects.
uintptr_t linked_image_base()
{
	HANDLE file = CreateFileA(executable, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return exe_base;
	unsigned char header[4096];
	DWORD got = 0;
	ReadFile(file, header, sizeof header, &got, NULL);
	CloseHandle(file);
	if (got < sizeof(IMAGE_DOS_HEADER))
		return exe_base;
	LONG at = ((IMAGE_DOS_HEADER*)header)->e_lfanew;
	if (at <= 0 || at + sizeof(IMAGE_NT_HEADERS64) > got)
		return exe_base;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(header + at);
	if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return (uintptr_t)nt->OptionalHeader.ImageBase;
	return (uintptr_t)((const IMAGE_NT_HEADERS32*)(header + at))->OptionalHeader.ImageBase;
}

void append(char* to, size_t size, const char* text)
{
	size_t at = strlen(to);
	while (*text && at + 1 < size)
		to[at++] = *text++;
	to[at] = 0;
}

void symbolise_frames()
{
	if (!symbolise_request || InterlockedDecrement(&symbolised_reports) < 0)
		return;
	command[0] = 0;
	append(command, sizeof command, "\"");
	append(command, sizeof command, addr2line);
	append(command, sizeof command, "\" -f -C -i -p -e \"");
	append(command, sizeof command, executable);
	append(command, sizeof command, "\"");
	int count = 0;
	for (int i = 0; i < frame_count; i++) {
		uintptr_t base;
		const char* name;
		if (!port_crash_module(frames[i], &base, &name) || base != exe_base)
			continue;
		// Return addresses point after the call; step back into it.
		PortCrashLine address;
		address.text(" ");
		address.hex(frames[i] - exe_base + exe_preferred_base - (i ? 1 : 0));
		address.bytes[address.length] = 0;
		append(command, sizeof command, address.bytes);
		count++;
	}
	if (!count)
		return;
	PortCrashLine line;
	line.text("[port] backtrace in "); line.text(executable); line.text(", symbolised by addr2line:\n");
	port_crash_emit(line);
	ResetEvent(symbolise_done);
	SetEvent(symbolise_request);
	if (WaitForSingleObject(symbolise_done, 10000) != WAIT_OBJECT_0) {
		line.text("[port] addr2line did not finish in time\n");
		port_crash_emit(line);
	}
}

void report_frame(int index, uintptr_t pc)
{
	if (index < kMaxFrames) {
		frames[index] = pc;
		frame_count = index + 1;
	}
	PortCrashLine line;
	line.text("[port]   #"); line.number(index, 10);
	line.text(" "); line.hex(pc);
	uintptr_t base;
	const char* name;
	if (port_crash_module(pc, &base, &name)) {
		line.text(" "); line.text(name);
		line.text("+"); line.hex(pc - base);
	}
	line.text("\n");
	port_crash_emit(line);
}

void report_stop(const char* why)
{
	PortCrashLine line;
	line.text("[port]   backtrace stops: "); line.text(why); line.text("\n");
	port_crash_emit(line);
}

// Walk the thread's stack within the TEB's bounds, which follow the switch
// onto a game thread's low stack. Never guess past a frame Windows cannot
// unwind: where the walk stops is part of the report.
void report_backtrace(const CONTEXT* start)
{
	frame_count = 0;
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	const uintptr_t low = (uintptr_t)tib->StackLimit, high = (uintptr_t)tib->StackBase;
	PortCrashLine line;
	line.text("[port] backtrace (stack "); line.hex(low); line.text("-"); line.hex(high); line.text("):\n");
	port_crash_emit(line);
#ifdef _WIN64
	// Frame pointers do not chain on Windows x64 (rbp points into the frame),
	// so unwind with the modules' own unwind data, one frame at a time.
	CONTEXT context = *start;
	for (int frame = 0; frame < kMaxFrames; frame++) {
		report_frame(frame, context.Rip);
		const DWORD64 sp = context.Rsp;
		DWORD64 image;
		PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, NULL);
		if (function) {
			void* handler_data;
			DWORD64 establisher;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &establisher, NULL);
		} else if (frame == 0 && sp >= low && sp + 8 <= high) {
			// A leaf function, or a call to a bad address: the return address
			// is on top of the stack.
			context.Rip = *(DWORD64*)sp;
			context.Rsp = sp + 8;
		} else {
			// port_win64_stack_call's entry thunk has no unwind data; it sits
			// just below the top of a game thread's low stack.
			report_stop(high - sp <= 64 ? "entry to the game thread's low stack"
			                            : "no unwind data for this address");
			return;
		}
		if (!context.Rip)
			return; // the thread's first frame
		if (context.Rsp <= sp || context.Rsp < low || context.Rsp >= high) {
			report_stop("the unwound stack pointer leaves the stack");
			return;
		}
	}
#else
	// 32-bit: every function the port builds keeps the ebp frame chain.
	report_frame(0, start->Eip);
	uintptr_t frame_pointer = start->Ebp;
	for (int frame = 1; frame < kMaxFrames; frame++) {
		if (frame_pointer < low || frame_pointer + 8 > high || (frame_pointer & 3)) {
			report_stop("the frame pointer leaves the stack");
			return;
		}
		const uintptr_t* saved = (const uintptr_t*)frame_pointer;
		if (!saved[1])
			return;
		report_frame(frame, saved[1]);
		if (saved[0] <= frame_pointer) {
			if (saved[0])
				report_stop("the frame chain does not go up the stack");
			return;
		}
		frame_pointer = saved[0];
	}
#endif
	report_stop("frame limit");
}

void report_registers(const CONTEXT* c)
{
	PortCrashLine line;
#ifdef _WIN64
	line.text("[port] registers:"); line.reg("rax", c->Rax); line.reg("rbx", c->Rbx);
	line.reg("rcx", c->Rcx); line.reg("rdx", c->Rdx); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("rsi", c->Rsi); line.reg("rdi", c->Rdi);
	line.reg("rbp", c->Rbp); line.reg("rsp", c->Rsp); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("r8", c->R8); line.reg("r9", c->R9);
	line.reg("r10", c->R10); line.reg("r11", c->R11); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("r12", c->R12); line.reg("r13", c->R13);
	line.reg("r14", c->R14); line.reg("r15", c->R15); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("rip", c->Rip); line.reg("eflags", c->EFlags);
	line.text("\n"); port_crash_emit(line);
#else
	line.text("[port] registers:"); line.reg("eax", c->Eax); line.reg("ebx", c->Ebx);
	line.reg("ecx", c->Ecx); line.reg("edx", c->Edx); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("esi", c->Esi); line.reg("edi", c->Edi);
	line.reg("ebp", c->Ebp); line.reg("esp", c->Esp); line.text("\n"); port_crash_emit(line);
	line.text("[port]  "); line.reg("eip", c->Eip); line.reg("eflags", c->EFlags);
	line.text("\n"); port_crash_emit(line);
#endif
}

// Everything after the first lines: shared by exceptions and abort().
void report_details(const CONTEXT* context, bool walk_stack)
{
#ifdef _WIN64
	const uintptr_t pc = context->Rip, sp = context->Rsp;
#else
	const uintptr_t pc = context->Eip, sp = context->Esp;
#endif
	report_registers(context);
	// The walk itself needs stack (a CONTEXT is over 1 KiB); an overflowed
	// stack has none to give.
	if (walk_stack)
		report_backtrace(context);
	port_crash_dump_memory(pc, sp);
	port_crash_describe_context();
	if (walk_stack)
		symbolise_frames();
}

LONG CALLBACK report_exception(EXCEPTION_POINTERS* exception)
{
	const EXCEPTION_RECORD* record = exception->ExceptionRecord;
	const DWORD code = record->ExceptionCode;
	volatile LONG* budget;
	bool is_throw = code == 0xE06D7363 || code == 0x20474343; // MSVC and GCC/libunwind C++ throws
	if (is_throw)
		budget = &throw_reports;
	else if ((code & 0xC0000000) == 0xC0000000)
		// Error severity: every hardware fault and fatal status, including
		// unwinding failures (0xC00000FF STATUS_BAD_FUNCTION_TABLE, 0xC0000028
		// STATUS_BAD_STACK). Lower severities are breakpoints, guard pages
		// (stack growth), debug output and thread names.
		budget = &fault_reports;
	else
		return EXCEPTION_CONTINUE_SEARCH;
	// MinGW can lazily allocate emulated thread_local storage. Use a process
	// guard instead so first-time crash reporting cannot touch a damaged heap.
	if (InterlockedCompareExchange(&reporting, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedDecrement(budget) < 0) {
		InterlockedExchange(&reporting, 0);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	PortCrashLine line;
	line.text("\n[port] Windows exception "); line.hex(code);
	line.text(" at address "); line.hex((uintptr_t)record->ExceptionAddress);
	line.text(" (first chance)\n");
	port_crash_emit(line);
	line.text("[port] exception thread "); line.number(GetCurrentThreadId(), 10);
#ifdef _WIN64
	line.text(" pc "); line.hex(exception->ContextRecord->Rip);
	line.text(" sp "); line.hex(exception->ContextRecord->Rsp);
	line.text(" bp "); line.hex(exception->ContextRecord->Rbp);
#else
	line.text(" pc "); line.hex(exception->ContextRecord->Eip);
	line.text(" sp "); line.hex(exception->ContextRecord->Esp);
	line.text(" bp "); line.hex(exception->ContextRecord->Ebp);
#endif
	line.text("\n");
	port_crash_emit(line);

	// Report the first lines before asking the loader for module information.
	uintptr_t base;
	const char* name;
	if (port_crash_module((uintptr_t)record->ExceptionAddress, &base, &name)) {
		line.text("[port] exception module "); line.text(name);
		line.text(" base "); line.hex(base);
		line.text(" offset "); line.hex((uintptr_t)record->ExceptionAddress - base);
		line.text("\n");
		port_crash_emit(line);
	}
	if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
		const ULONG_PTR kind = record->ExceptionInformation[0];
		line.text("[port] access "); line.number(kind, 10);
		line.text(" target "); line.hex(record->ExceptionInformation[1]);
		line.text(kind == 0 ? " (a read)" : kind == 1 ? " (a write)" : kind == 8 ? " (an instruction fetch, DEP)" : "");
		line.text("\n");
		port_crash_emit(line);
		port_crash_describe_address("fault address", record->ExceptionInformation[1]);
	} else if (code == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3) {
		line.text("[port] I/O status "); line.hex(record->ExceptionInformation[2]);
		line.text("\n");
		port_crash_emit(line);
	} else if (record->NumberParameters) {
		line.text("[port] exception parameters");
		for (DWORD i = 0; i < record->NumberParameters && i < 4; i++) {
			line.text(" "); line.hex(record->ExceptionInformation[i]);
		}
		line.text("\n");
		port_crash_emit(line);
	}
	// An exception raised while another was being handled (an unwind that
	// failed, say) carries the one it interrupted.
	const EXCEPTION_RECORD* nested = record->ExceptionRecord;
	for (int depth = 0; nested && depth < 4; depth++, nested = nested->ExceptionRecord) {
		line.text("[port] raised while handling exception "); line.hex(nested->ExceptionCode);
		line.text(" at address "); line.hex((uintptr_t)nested->ExceptionAddress);
		if (port_crash_module((uintptr_t)nested->ExceptionAddress, &base, &name)) {
			line.text(" ("); line.text(name); line.text("+"); line.hex((uintptr_t)nested->ExceptionAddress - base);
			line.text(")");
		}
		line.text("\n");
		port_crash_emit(line);
	}
	if (is_throw) {
		// A throw is often caught; its registers and stack are enough.
		report_backtrace(exception->ContextRecord);
	} else {
		report_details(exception->ContextRecord, code != EXCEPTION_STACK_OVERFLOW);
	}
	InterlockedExchange(&reporting, 0);
	// This is a first-chance diagnostic, not proof the exception is unhandled.
	// Let Windows retain its normal handling and original process exit status.
	// __fastfail/RaiseFailFastException may bypass this logger entirely.
	return EXCEPTION_CONTINUE_SEARCH;
}

// Reached only when nothing handled the exception: says which of the reports
// above ended the process.
LONG WINAPI report_unhandled(EXCEPTION_POINTERS* exception)
{
	PortCrashLine line;
	line.text("[port] unhandled exception "); line.hex(exception->ExceptionRecord->ExceptionCode);
	line.text(" at address "); line.hex((uintptr_t)exception->ExceptionRecord->ExceptionAddress);
	line.text(": the process ends\n");
	port_crash_emit(line);
	return previous_filter ? previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void port_windows_report_current(const char* what)
{
	if (InterlockedCompareExchange(&reporting, 1, 0)) return;
	CONTEXT context;
	RtlCaptureContext(&context);
	PortCrashLine line;
	line.text("\n[port] fatal: "); line.text(what);
	line.text(" in thread "); line.number(GetCurrentThreadId(), 10); line.text("\n");
	port_crash_emit(line);
	report_details(&context, true);
	InterlockedExchange(&reporting, 0);
}

void port_install_windows_exception_logger()
{
	static bool installed;
	if (installed) return;
	port_crash_prepare_info();
	GetModuleFileNameA(NULL, executable, sizeof executable);
	executable[sizeof executable - 1] = 0;
	exe_base = (uintptr_t)GetModuleHandleA(NULL);
	exe_preferred_base = linked_image_base();
	if (const char* tool = getenv("SMS_ADDR2LINE")) {
		addr2line[0] = 0;
		append(addr2line, sizeof addr2line, tool);
	}
	HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
	if (error && error != INVALID_HANDLE_VALUE &&
	    DuplicateHandle(GetCurrentProcess(), error, GetCurrentProcess(), &inheritable_error, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
		symbolise_request = CreateEventA(NULL, FALSE, FALSE, NULL);
		symbolise_done = CreateEventA(NULL, TRUE, FALSE, NULL);
		HANDLE thread = symbolise_request && symbolise_done ? CreateThread(NULL, 64 * 1024, symboliser, NULL, 0, NULL) : NULL;
		if (thread)
			CloseHandle(thread);
		else
			symbolise_request = NULL;
	}
	installed = AddVectoredExceptionHandler(1, report_exception) != NULL;
	if (!installed) fprintf(stderr, "[port] cannot install Windows exception logger: %lu\n", GetLastError());
	previous_filter = SetUnhandledExceptionFilter(report_unhandled);
}
#endif
