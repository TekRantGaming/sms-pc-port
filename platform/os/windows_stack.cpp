#ifdef _WIN64
#include "port_win64_stack.h"
#include <windows.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
thread_local jmp_buf* exit_point;
struct Call { void* (*entry)(void*); void* argument; };

void* invoke(void* argument)
{
	Call* call = (Call*)argument;
	jmp_buf done;
	jmp_buf* previous = exit_point;
	exit_point = &done;
	void* result = NULL;
	if (setjmp(done) == 0)
		result = call->entry(call->argument);
	exit_point = previous;
	return result;
}
}

// Windows x64 ABI: rcx=top, rdx=callback, r8=argument. Preserve the old
// stack in the callee-saved rbx and provide the callback's 32-byte home area.
extern "C" void* sms_win64_stack_enter(void*, void* (*)(void*), void*);
__asm__(
	".text\n"
	".globl sms_win64_stack_enter\n"
	"sms_win64_stack_enter:\n"
	"push %rbx\n"
	"mov %rsp, %rbx\n"
	"mov %rcx, %rsp\n"
	"and $-16, %rsp\n"
	"sub $32, %rsp\n"
	"mov %r8, %rcx\n"
	"call *%rdx\n"
	"mov %rbx, %rsp\n"
	"pop %rbx\n"
	"ret\n");

void* port_win64_stack_call(void* stack, size_t size, void* (*entry)(void*), void* argument)
{
	if (!stack || size < 65536 || (uintptr_t)stack + size > 0x80000000ULL) {
		fprintf(stderr, "[port] invalid low Windows stack\n");
		abort();
	}
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	void* original_base = tib->StackBase;
	void* original_limit = tib->StackLimit;
	Call call = {entry, argument};
	tib->StackBase = (char*)stack + size;
	tib->StackLimit = stack;
	void* result = sms_win64_stack_enter(tib->StackBase, invoke, &call);
	tib->StackBase = original_base;
	tib->StackLimit = original_limit;
	return result;
}

void port_win64_thread_exit(void)
{
	if (!exit_point) {
		fprintf(stderr, "[port] thread exit outside a low Windows stack\n");
		abort();
	}
	// Return within this low stack first. winpthreads receives an ordinary
	// callback return on its original stack and performs its normal cleanup.
	longjmp(*exit_point, 1);
}
#endif
