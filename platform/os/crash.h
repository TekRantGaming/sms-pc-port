#pragma once
#include <stddef.h>
#include <stdint.h>

struct PortCrashLine;

// Install process handlers before boot or switching to the game's low stack.
void port_install_crash_handlers();
// Alternate signal stacks are per host thread on POSIX.
void port_crash_thread_init();
#ifdef _WIN32
void port_install_windows_exception_logger();
// A full report (registers, backtrace, context) of the calling thread, for
// paths that end the process without an exception, such as abort().
void port_windows_report_current(const char* what);
#endif

// Report sections shared by every platform (crash_info.cpp). None allocates
// or takes a lock the crashed thread may hold.
// Writes the line to stderr and empties it.
void port_crash_emit(PortCrashLine& line);
// Copies memory that may be unmapped; false (and nothing copied) if it is.
bool port_crash_read(void* to, uintptr_t from, size_t size);
// The module holding `address`: its base and file name without the folder.
bool port_crash_module(uintptr_t address, uintptr_t* base, const char** name);
// Gathers the build and system description early, while that is still safe.
void port_crash_prepare_info();
// The executable's path, as found by port_crash_prepare_info ("" if unknown).
const char* port_crash_executable();
// What a faulting address points into (null page, emulated MEM1, ...).
void port_crash_describe_address(const char* label, uintptr_t address);
// The instruction bytes around pc and the words at the top of the stack.
void port_crash_dump_memory(uintptr_t pc, uintptr_t sp);
// The emulated OS threads, recent disc files, build and system.
void port_crash_describe_context();
// Remembered for the report: the disc files the game asked for last.
void port_crash_note_file(const char* path);
// Emulated MEM1, for describing addresses.
void port_crash_set_mem1(uintptr_t base, size_t size);
// The OS layer describes its threads (registered so tests can omit it).
typedef void (*PortCrashDescriber)();
void port_crash_set_thread_describer(PortCrashDescriber describer);
