#ifdef _WIN32
#include "crash.h"
#include "crash_line.h"
#include <windows.h>
#include <stdio.h>
#include <io.h>

namespace {
HANDLE error_output = INVALID_HANDLE_VALUE;
volatile LONG reporting;

LONG CALLBACK report_exception(EXCEPTION_POINTERS* exception)
{
	const EXCEPTION_RECORD* record = exception->ExceptionRecord;
	switch (record->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case 0xC0000017: // STATUS_NO_MEMORY
	case 0xC0000374: // STATUS_HEAP_CORRUPTION
	case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (fast-fail)
	case 0xC0000602: // STATUS_FAIL_FAST_EXCEPTION
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH; // C++ throws, debugger traps, guard pages
	}
	// MinGW can lazily allocate emulated thread_local storage. Use a process
	// guard instead so first-time crash reporting cannot touch a damaged heap.
	if (InterlockedCompareExchange(&reporting, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
	PortCrashLine line;
	line.text("[port] Windows exception "); line.hex(record->ExceptionCode);
	line.text(" at address "); line.hex((uintptr_t)record->ExceptionAddress);
	line.text(" (first chance)\n");
	DWORD written;
	WriteFile(error_output, line.bytes, (DWORD)line.length, &written, NULL);
	line.length = 0;
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
	WriteFile(error_output, line.bytes, (DWORD)line.length, &written, NULL);

	// Report the first line before asking the loader for module information.
	// No unwind is attempted across the custom Windows x64 stack thunk.
	MEMORY_BASIC_INFORMATION memory;
	if (VirtualQuery(record->ExceptionAddress, &memory, sizeof memory)) {
		char module[MAX_PATH] = {};
		GetModuleFileNameA((HMODULE)memory.AllocationBase, module, sizeof module);
		module[sizeof module - 1] = 0;
		line.length = 0;
		line.text("[port] exception module "); line.text(*module ? module : "<unknown>");
		line.text(" base "); line.hex((uintptr_t)memory.AllocationBase);
		line.text(" offset "); line.hex((uintptr_t)record->ExceptionAddress - (uintptr_t)memory.AllocationBase);
		if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
			line.text(" access "); line.number(record->ExceptionInformation[0], 10);
			line.text(" target "); line.hex(record->ExceptionInformation[1]);
		}
		if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3) {
			line.text(" I/O status "); line.hex(record->ExceptionInformation[2]);
		}
		line.text("\n");
		WriteFile(error_output, line.bytes, (DWORD)line.length, &written, NULL);
	}
	InterlockedExchange(&reporting, 0);
	// This is a first-chance diagnostic, not proof the exception is unhandled.
	// Let Windows retain its normal handling and original process exit status.
	// __fastfail/RaiseFailFastException may bypass this logger entirely.
	return EXCEPTION_CONTINUE_SEARCH;
}
}

void port_install_windows_exception_logger()
{
	static bool installed;
	if (installed) return;
	error_output = (HANDLE)_get_osfhandle(_fileno(stderr));
	if (error_output == INVALID_HANDLE_VALUE) error_output = GetStdHandle(STD_ERROR_HANDLE);
	installed = AddVectoredExceptionHandler(1, report_exception) != NULL;
	if (!installed) fprintf(stderr, "[port] cannot install Windows exception logger: %lu\n", GetLastError());
}
#endif
