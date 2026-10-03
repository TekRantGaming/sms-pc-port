/* Port override of decomp/libs/dolphin/include/dolphin/types.h (same include guard, so it
 * wins whichever copy is reached first; port_compat.h includes this one).
 * MWCC's u32/s32 are (unsigned) long; on LP64 hosts that is 64 bits, so the
 * port spells them int on all 64-bit hosts (including Windows LLP64), so its platform signatures agree. On ILP32 hosts long is 32 bits and the original
 * spelling (and C++ mangling) is kept. */
#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_

typedef signed char s8;
typedef unsigned char u8;
typedef signed short int s16;
typedef unsigned short int u16;
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
typedef signed int s32;
typedef unsigned int u32;
#else
typedef signed long s32;
typedef unsigned long u32;
#endif
#if defined(__i386__)
/* The GameCube aligns 8-byte members to 8 inside structs; the i386 psABI
 * aligns them to 4, which would lay out every class holding an OSTime or an
 * f64 after a 4-byte boundary (TMarDirector, JUTGamePad, OSThread...)
 * differently from retail. Code mods address those classes with retail
 * offsets, so the 32-bit port keeps retail's alignment for these types.
 * Argument passing is unaffected. */
typedef signed long long int s64 __attribute__((aligned(8)));
typedef unsigned long long int u64 __attribute__((aligned(8)));
#else
typedef signed long long int s64;
typedef unsigned long long int u64;
#endif

typedef float f32;
#if defined(__i386__)
typedef double f64 __attribute__((aligned(8)));
#else
typedef double f64;
#endif

typedef char* Ptr;

typedef int BOOL;

#define FALSE 0
#define TRUE  1

/* Hardware-mapped objects (low-memory OS globals, hw_regs.h register blocks)
 * are *defined* in SDK headers. On the host they become ordinary weak
 * globals, so the per-TU definitions merge at link time; the platform layer
 * initialises the ones that matter (bus clock, memory size, TV mode). */
#ifdef _WIN32
/* COFF has no ELF-style weak data. selectany puts header definitions in
 * COMDAT sections so the linker keeps one copy shared by all game units. */
#define AT_ADDRESS(addr) __attribute__((selectany))
#else
#define AT_ADDRESS(addr) __attribute__((weak))
#endif

#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

/* PTR32(T): a pointer field of a struct laid over file data (see the decomp's
 * types.h). With 4-byte pointers it is T*. With 8-byte pointers it stays a
 * 4-byte slot holding the address, which works because everything the game
 * can point at lives below 4 GiB (MEM1, static data, low thread stacks); a
 * higher address is a port bug, reported by port_ptr32_trap. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#ifdef __cplusplus
extern "C" void port_ptr32_trap(const void* p);
template <typename T> struct port_ptr32 {
	u32 raw;
	operator T*() const { return (T*)(unsigned long long)raw; }
	T* operator->() const { return (T*)(unsigned long long)raw; }
	port_ptr32& operator=(T* p)
	{
		unsigned long long a = (unsigned long long)p;
		if (a >> 32)
			port_ptr32_trap(p);
		raw = (u32)a;
		return *this;
	}
	/* pointer arithmetic, as on T* (in-place relocation adds a base) */
	template <typename I> port_ptr32& operator+=(I n) { return *this = (T*)*this + n; }
	template <typename I> port_ptr32& operator-=(I n) { return *this = (T*)*this - n; }
};
#define PTR32(T) port_ptr32<T>
#else
#define PTR32(T) u32
#endif
#else
#define PTR32(T) T*
#endif

#endif
