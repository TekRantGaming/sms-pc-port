#ifdef _WIN64
#include "port_win64_stack.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
// Where a thread leaving its low stack resumes: the original stack pointer
// inside sms_win64_stack_enter, below the registers it saved.
struct ExitPoint { void* sp; };
thread_local ExitPoint* exit_point;
}

// Windows x64 ABI: rcx=top, rdx=entry, r8=argument, r9=exit point. Save the
// callee-saved registers (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15) on the
// original stack, record that stack pointer for a thread exit, and keep it
// in rbx across the call, which also gets its 32-byte home area.
//
// A thread exit (sms_win64_stack_leave) restores that stack pointer and
// returns through the same epilogue. The CRT's longjmp, used here before,
// asks Windows to unwind every frame in between (RtlUnwindEx); that needs
// unwind data each frame can be followed by, and stops the process with
// 0xC00000FF (STATUS_BAD_FUNCTION_TABLE) otherwise: Super Mario Eclipse
// cancels a thread blocked in its own code at every stage exit, and the
// unwind through that frame failed. The frames being left belong to a thread
// that ends; game code has no destructors to run.
extern "C" void* sms_win64_stack_enter(void*, void* (*)(void*), void*, ExitPoint*);
extern "C" __attribute__((noreturn)) void sms_win64_stack_leave(ExitPoint*);
__asm__(
	".text\n"
	".globl sms_win64_stack_enter\n"
	"sms_win64_stack_enter:\n"
	"push %rbx\n"
	"push %rbp\n"
	"push %rdi\n"
	"push %rsi\n"
	"push %r12\n"
	"push %r13\n"
	"push %r14\n"
	"push %r15\n"
	// 160 bytes for xmm6-xmm15 and 8 to align them to 16
	"sub $168, %rsp\n"
	"movaps %xmm6, 0(%rsp)\n"
	"movaps %xmm7, 16(%rsp)\n"
	"movaps %xmm8, 32(%rsp)\n"
	"movaps %xmm9, 48(%rsp)\n"
	"movaps %xmm10, 64(%rsp)\n"
	"movaps %xmm11, 80(%rsp)\n"
	"movaps %xmm12, 96(%rsp)\n"
	"movaps %xmm13, 112(%rsp)\n"
	"movaps %xmm14, 128(%rsp)\n"
	"movaps %xmm15, 144(%rsp)\n"
	"mov %rsp, (%r9)\n"
	"mov %rsp, %rbx\n"
	"mov %rcx, %rsp\n"
	"and $-16, %rsp\n"
	"sub $32, %rsp\n"
	"mov %r8, %rcx\n"
	"call *%rdx\n"
	"mov %rbx, %rsp\n"
	"sms_win64_stack_restore:\n"
	"movaps 0(%rsp), %xmm6\n"
	"movaps 16(%rsp), %xmm7\n"
	"movaps 32(%rsp), %xmm8\n"
	"movaps 48(%rsp), %xmm9\n"
	"movaps 64(%rsp), %xmm10\n"
	"movaps 80(%rsp), %xmm11\n"
	"movaps 96(%rsp), %xmm12\n"
	"movaps 112(%rsp), %xmm13\n"
	"movaps 128(%rsp), %xmm14\n"
	"movaps 144(%rsp), %xmm15\n"
	"add $168, %rsp\n"
	"pop %r15\n"
	"pop %r14\n"
	"pop %r13\n"
	"pop %r12\n"
	"pop %rsi\n"
	"pop %rdi\n"
	"pop %rbp\n"
	"pop %rbx\n"
	"ret\n"
	// rcx=exit point: back onto the original stack, returning NULL
	".globl sms_win64_stack_leave\n"
	"sms_win64_stack_leave:\n"
	"mov (%rcx), %rsp\n"
	"xor %eax, %eax\n"
	"jmp sms_win64_stack_restore\n");

void* port_win64_stack_call(void* stack, size_t size, void* (*entry)(void*), void* argument)
{
	if (!stack || size < 65536 || (uintptr_t)stack + size > 0x80000000ULL) {
		fprintf(stderr, "[port] invalid low Windows stack\n");
		abort();
	}
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	void* original_base = tib->StackBase;
	void* original_limit = tib->StackLimit;
	ExitPoint exit = {NULL};
	ExitPoint* previous = exit_point;
	exit_point = &exit;
	tib->StackBase = (char*)stack + size;
	tib->StackLimit = stack;
	void* result = sms_win64_stack_enter(tib->StackBase, entry, argument, &exit);
	tib->StackBase = original_base;
	tib->StackLimit = original_limit;
	exit_point = previous;
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
	sms_win64_stack_leave(exit_point);
}
#endif
