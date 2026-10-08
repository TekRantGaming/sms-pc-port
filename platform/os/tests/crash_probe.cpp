// ROM-free crash probe. Link the runtime with --gc-sections so only its crash
// reporting is used. The Windows logger can be linked without the runtime.
#include "../crash.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include "port_win64_stack.h"
#else
#include <pthread.h>
#include <unistd.h>
extern "C" int gcdisc_self_path(char*, uint32_t) { return 0; }
#endif

#ifndef _WIN32
__attribute__((noinline)) static unsigned exhaust_stack(unsigned depth)
{
	volatile unsigned char local[4096];
	memset((void*)local, depth, sizeof local);
	return exhaust_stack(depth + 1) + local[depth % sizeof local];
}
#endif

static void* fault(void*)
{
#ifndef _WIN32
	port_crash_thread_init();
#endif
	*(volatile int*)(uintptr_t)1 = 42;
	return NULL;
}

#ifndef _WIN32
static void* overflow(void*)
{
	port_crash_thread_init();
	return (void*)(uintptr_t)exhaust_stack(0);
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
	port_install_windows_exception_logger();
	SetErrorMode(SEM_NOGPFAULTERRORBOX);
#else
	port_install_crash_handlers();
#endif
	if (argc < 2) return 2;
	if (!strcmp(argv[1], "access")) fault(NULL);
#ifdef _WIN32
	if (!strcmp(argv[1], "heap")) RaiseException(0xC0000374, EXCEPTION_NONCONTINUABLE, 0, NULL);
#ifdef _WIN64
	if (!strcmp(argv[1], "low-stack")) {
		void* stack = VirtualAlloc((void*)0x10000000, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!stack) return 3;
		port_win64_stack_call(stack, 1 << 20, fault, NULL);
	}
#endif
#else
	if (!strcmp(argv[1], "abort")) raise(SIGABRT);
	if (!strcmp(argv[1], "thread")) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, fault, NULL)) return 3;
		pthread_join(thread, NULL);
	}
	if (!strcmp(argv[1], "overflow")) {
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, 256 * 1024);
		pthread_t thread;
		if (pthread_create(&thread, &attributes, overflow, NULL)) return 3;
		pthread_attr_destroy(&attributes);
		pthread_join(thread, NULL);
	}
#endif
	return 4; // A requested crash should never return.
}
