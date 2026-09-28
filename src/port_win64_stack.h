#ifndef SMS_PORT_WIN64_STACK_H
#define SMS_PORT_WIN64_STACK_H
#include <stddef.h>
#ifdef _WIN64
// winpthreads accepts pthread_attr_setstack but ignores its address. Run the
// game callback on an explicitly allocated low stack, with matching TEB bounds.
void* port_win64_stack_call(void* stack, size_t size, void* (*entry)(void*), void* argument);
void port_win64_thread_exit(void) __attribute__((noreturn));
#endif
#endif
