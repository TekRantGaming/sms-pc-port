// ROM-free Windows x64 runtime check: stack addresses, TEB bounds, thread
// exit, repeated creation/join, and an ordinary host allocation afterwards.
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
static void* check(void* exit)
{
	volatile int local = 1;
	uintptr_t address = (uintptr_t)&local;
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	require(address >= (uintptr_t)stack && address < (uintptr_t)stack + size);
	require(tib->StackBase == (char*)stack + size && tib->StackLimit == stack);
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
		require(pthread_create(&th, NULL, thread, (void*)1) == 0);
		void* result = (void*)42;
		require(pthread_join(th, &result) == 0 && result == NULL);
	}
	VirtualFree(stack, 0, MEM_RELEASE);
	int* host = new int(123);
	require(*host == 123);
	delete host;
	puts("Windows x64 low-stack runtime check passed");
}
