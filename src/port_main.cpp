// Host entry point: sets up the platform layer, then enters the game's main().
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "port_platform.h"
#include "port_win64_stack.h"

extern void SMS_main(void);

struct Args {
	int argc;
	char** argv;
};

static void* run_game(void* p)
{
	Args* a = (Args*)p;
	port_init(a->argc, a->argv);
	std::fprintf(stderr, "[port] entering SMS_main\n");
	SMS_main();
	std::fprintf(stderr, "[port] SMS_main returned\n");
	return NULL;
}

#if UINTPTR_MAX > 0xFFFFFFFFu
#include <pthread.h>

// 64-bit hosts: the game keeps pointers to its locals in u32 slots, so the
// boot thread (which becomes the default OSThread) runs on a stack below
// 2 GiB instead of the process's main stack.
int main(int argc, char** argv)
{
	const unsigned long size = 8ul << 20;
	void* stack = port_low_alloc(size);
	if (!stack) {
		std::fprintf(stderr, "[port] no memory below 2 GiB for the game's stack\n");
#ifdef __APPLE__
		std::fprintf(stderr,
			"[port] on Apple Silicon, build the x86_64 (Rosetta) binary with ./build.sh;\n"
			"[port] native arm64 reserves the low 4 GiB as PAGEZERO and cannot mmap there.\n");
#endif
		return 1;
	}
	Args a = {argc, argv};
#ifdef _WIN64
	port_win64_stack_call(stack, size, run_game, &a);
	return 0;
#endif
#if defined(__APPLE__) && defined(__x86_64__)
	// AppKit (SDL's window and event pump) only works on the process's main
	// thread, so instead of a second thread, switch this one onto the low
	// stack. Callee-saved rbx holds the old rsp across the call.
	void* top = (char*)stack + size;
	__asm__ volatile(
		"mov %%rsp, %%rbx\n\t"
		"mov %0, %%rsp\n\t"
		"mov %2, %%rdi\n\t"
		"call *%1\n\t"
		"mov %%rbx, %%rsp"
		:
		: "r"(top), "r"(run_game), "r"(&a)
		: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
		  "memory", "cc");
	return 0;
#endif
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstack(&attr, stack, size);
	pthread_t th;
	if (pthread_create(&th, &attr, run_game, &a) != 0) {
		std::fprintf(stderr, "[port] cannot start the game thread\n");
		return 1;
	}
	pthread_attr_destroy(&attr);
	pthread_join(th, NULL);
	return 0;
}
#else
int main(int argc, char** argv)
{
	Args a = {argc, argv};
	run_game(&a);
	return 0;
}
#endif
