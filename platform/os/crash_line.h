#pragma once
#include <stdint.h>
#include <stddef.h>

// Crash reporting must not allocate or acquire the stdio/locale locks: the
// fault may have occurred inside an allocator or while another thread logs.
struct PortCrashLine {
	char bytes[1024];
	size_t length = 0;
	void text(const char* value) {
		while (*value && length < sizeof bytes) bytes[length++] = *value++;
	}
	void number(uint64_t value, unsigned base = 16) {
		char digits[32];
		unsigned count = 0;
		do { digits[count++] = "0123456789ABCDEF"[value % base]; value /= base; } while (value);
		while (count && length < sizeof bytes) bytes[length++] = digits[--count];
	}
	void hex(uint64_t value) { text("0x"); number(value); }
};
