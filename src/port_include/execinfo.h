#ifndef SMS_PORT_EXECINFO_H
#define SMS_PORT_EXECINFO_H

#ifdef _WIN32
#undef __declspec
#define __declspec(x) __attribute__((x))
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static inline int backtrace(void** frames, int capacity)
{
	return (int)CaptureStackBackTrace(0, (DWORD)capacity, frames, NULL);
}
static inline void backtrace_symbols_fd(void* const* frames, int count, int)
{
	for (int i = 0; i < count; ++i)
		fprintf(stderr, "[port] frame %d: %p\n", i, frames[i]);
}
static inline char** backtrace_symbols(void* const* frames, int count)
{
	if (count <= 0) return NULL;
	char** names = (char**)malloc((size_t)count * (sizeof(char*) + 32));
	if (!names) return NULL;
	char* text = (char*)(names + count);
	for (int i = 0; i < count; ++i) {
		names[i] = text + (size_t)i * 32;
		snprintf(names[i], 32, "%p", frames[i]);
	}
	return names;
}
#else
#include_next <execinfo.h>
#endif

#endif
