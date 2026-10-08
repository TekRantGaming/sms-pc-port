// ROM-free Windows x64 runtime check: stack addresses, TEB bounds, thread
// exit (also through a frame without unwind data), repeated creation/join,
// and an ordinary host allocation afterwards.
#include "port_win64_stack.h"
#include <windows.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void require(bool condition)
{
	if (!condition) { fprintf(stderr, "Windows x64 low-stack check failed\n"); abort(); }
}
static void* stack;
static const size_t size = 1 << 20;
// A frame Windows cannot unwind, with garbage where an unwinder would look
// for the return address: a cancelled Eclipse thread exits through one of
// its own frames, and an unwinding exit stopped the game with 0xC00000FF.
extern "C" void frame_without_unwind_data(void (*fn)(void));
__asm__(".text\n.globl frame_without_unwind_data\nframe_without_unwind_data:\n"
        "push %rbx\nsub $0x40, %rsp\nmovq $0xDA4, 0x48(%rsp)\n"
        "call *%rcx\nadd $0x40, %rsp\npop %rbx\nret\n");
static void leave_thread(void) { port_win64_thread_exit(); }
static void* check(void* exit)
{
	volatile int local = 1;
	uintptr_t address = (uintptr_t)&local;
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	require(address >= (uintptr_t)stack && address < (uintptr_t)stack + size);
	require(tib->StackBase == (char*)stack + size && tib->StackLimit == stack);
	if (exit == (void*)2) frame_without_unwind_data(leave_thread);
	if (exit) port_win64_thread_exit();
	return (void*)42;
}
static void* thread(void* exit)
{
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	void* base = tib->StackBase;
	void* limit = tib->StackLimit;
	void* result = port_win64_stack_call(stack, size, check, exit);
	require(tib->StackBase == base && tib->StackLimit == limit);
	return result;
}
int main()
{
	require(sizeof(void*) == 8 && sizeof(long) == 4);
	require((uintptr_t)(&main) < 0x100000000ULL);
	stack = VirtualAlloc((void*)0x10000000, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	require(stack != NULL);
	require(thread(NULL) == (void*)42);
	for (int i = 0; i < 64; ++i) {
		pthread_t th;
		require(pthread_create(&th, NULL, thread, (void*)(uintptr_t)(1 + i % 2)) == 0);
		void* result = (void*)42;
		require(pthread_join(th, &result) == 0 && result == NULL);
	}
	VirtualFree(stack, 0, MEM_RELEASE);
	int* host = new int(123);
	require(*host == 123);
	delete host;
	puts("Windows x64 low-stack runtime check passed");
}
