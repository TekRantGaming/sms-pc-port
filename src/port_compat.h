/* Force-included (-include) into every decomp translation unit in the port
 * build. It stands in for the parts of MWCC and MSL the game relies on
 * implicitly, so that the game/JSystem sources compile unmodified with the
 * host g++/gcc and the host libc/libstdc++. */
#ifndef SMS_PORT_COMPAT_H
#define SMS_PORT_COMPAT_H

#ifndef TARGET_PC
#define TARGET_PC 1
#endif

/* Host C library first, before any macro below can disturb it. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <float.h>
#include <wchar.h>
#ifdef __cplusplus
#include <new>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <utility>
#endif

/* The decomp's configure.py passes -Dnullptr=0: nullptr is a plain 0, which
 * the game also assigns to integer fields (MActorAnmBck::unk28). Before C++11
 * it is no keyword (libc++ on macOS defines it as __nullptr in C++03 mode). */
#if !defined(__cplusplus) || __cplusplus < 201103L
#undef nullptr
#define nullptr 0
#endif

#include <dolphin/types.h> /* the port override (see port_include/) */

/* C++03 game code uses JSU's EOF enum. Modern platform code needs the
 * host EOF macro in libc++; its JSU header wrapper protects the enum. */
#if !defined(__cplusplus) || __cplusplus < 201103L
#undef EOF
#endif

/* MSL's rand(): RAND_MAX is 32767 (the game computes 1.f / (RAND_MAX + 1)
 * in int, which overflows with glibc's 2^31-1) and the sequence is the ANSI C
 * reference LCG. platform/misc/msl_rand.c provides it. */
#ifdef __cplusplus
extern "C" {
#endif
int sms_msl_rand(void);
void sms_msl_srand(unsigned int seed);
#ifdef __cplusplus
}
#endif
#undef RAND_MAX
#define RAND_MAX 32767
#define rand sms_msl_rand
#define srand sms_msl_srand

/* MWCC-only syntax. */
#define __declspec(x)
/* Whole-function `asm` definitions keep their Gekko bodies inside
 * #ifdef __MWERKS__, so dropping the keyword leaves a (C-less) body. The host
 * headers above use __asm__, never bare asm. */
#define asm
#define __sync() ((void)0)
#define __isync() ((void)0)

/* The game's entry point is `void main(void)`. */
#define main SMS_main

/* Gekko intrinsics. frsqrte and fres are the hardware's table estimates,
 * reproduced bit-exactly (src/port_fpu.h, measured with tools/fpprobe): the
 * game uses some of them without refinement (THitActor::calcEntryRadius), so
 * exact results would change collision radii. */
#include "port_fpu.h"
#ifdef __cplusplus
extern "C++" {
#endif
static inline u32 __cntlzw(u32 x) { return x ? (u32)__builtin_clz(x) : 32u; }
static inline double __frsqrte(double x) { return port_gekko_frsqrte(x); }
static inline float __fres(float x) { return port_gekko_fres(x); }
#ifdef __cplusplus
}
#endif
/* MWCC converts a float or double to a 32-bit unsigned integer through the
 * runtime's __cvt_fp2unsigned, and to a 64-bit one through __cvt_dbl_usll;
 * port_cvt_fp2unsigned and port_cvt_dbl_usll (port_fpu.h) give their
 * results, and every such conversion the DOL makes goes through them
 * (decomp-patches/fpu-02..04). port_cvt_fp<T> is the conversion for a
 * template whose T may be u32: the runtime's for a 32-bit unsigned T, a plain
 * cast (fctiwz and the low bits on the console, as on the host) otherwise. */
#ifdef __cplusplus
extern "C++" {
template <class T> struct port_cvt_fp_impl {
	template <class F> static T cvt(F x) { return (T)x; }
};
template <> struct port_cvt_fp_impl<unsigned int> {
	static unsigned int cvt(double d) { return port_cvt_fp2unsigned(d); }
};
template <> struct port_cvt_fp_impl<unsigned long> {
	static unsigned long cvt(double d) { return port_cvt_fp2unsigned(d); }
};
template <class T, class F> inline T port_cvt_fp(F x) { return port_cvt_fp_impl<T>::cvt(x); }
}
#endif
/* JSystem's and the game's paired-single routines outside MTX/VEC, as the
 * console computes them (platform/mtx/jsys_ps.inc). */
#include "port_ps.h"

/* Non-standard names MSL's math.h provides. MSL spells M_PI as a float. */
#undef M_PI
#define M_PI       3.14159265358979323846f
#define LONG_TAU   6.2831854820251465
#define TAU        6.2831855f
#define HALF_PI    1.5707964f
#define THIRD_PI   1.0471976f
#define QUARTER_PI 0.7853982f
#define SIN_2_5    0.43633234f
#define M_SQRT3    1.73205f
#define DEG_TO_RAD(degrees) (degrees * (M_PI / 180.0f))
#define RAD_TO_DEG(radians) (radians * (180.0f / M_PI))

#ifdef __cplusplus
/* MSL puts the C99 float functions in namespace std; libstdc++ in C++03 mode
 * does not. */
namespace std {
using ::sqrtf; using ::powf; using ::fmodf; using ::fabsf; using ::sinf;
using ::cosf; using ::tanf; using ::atanf; using ::atan2f; using ::acosf;
using ::asinf; using ::expf; using ::logf; using ::floorf; using ::ceilf;
using ::log10f; using ::snprintf; using ::vsnprintf;
}
/* MSL C++ adds float overloads of abs/sin/cos/atan2 at global scope. */
using std::abs;
using std::sin;
using std::cos;
using std::atan2;
using std::sqrt;
using std::fabs;
using std::floor;
using std::fmod;
using std::pow;
#endif

/* MSL's maths. The game calls the maths library in its DOL, whose results
 * differ from every host libm (and i386 glibc differs from x86-64 glibc), so
 * its calls go to the same code compiled for the host
 * (platform/misc/msl_math.c), with MSL's overloads:
 *   sinf cosf tanf atanf atan2f acosf  MSL's float functions (C and C++)
 *   expf powf
 *   atan2(double, double)             fdlibm's atan2
 *   C++ sin(float) cos(float)         MSL's float overloads: sinf, cosf and
 *       atan2(float, float)           atan2f
 *   C++ std::atan2f                   MSL's is ::atan2((double)y, (double)x)
 *   sqrtf, std::sqrtf, std::fmodf     MSL's header inlines (the console's
 *                                     sqrtf returns x for x <= 0 and NaN,
 *                                     and a NaN for +inf)
 * They are function-like macros, so the game's variables named sin or tan
 * keep their names; <math.h> and <cmath> are already included above. */
#include "msl_math.h"
#ifdef __cplusplus
static inline float sms_msl_sin(float x) { return sms_msl_sinf(x); }
static inline double sms_msl_sin(double x) { return ::sin(x); } /* no sin(double) in the DOL */
static inline float sms_msl_cos(float x) { return sms_msl_cosf(x); }
static inline double sms_msl_cos(double x) { return ::cos(x); } /* no cos(double) in the DOL */
static inline float sms_msl_atan2(float y, float x) { return sms_msl_atan2f(y, x); }
namespace std {
using ::sms_msl_sinf; using ::sms_msl_cosf; using ::sms_msl_tanf; using ::sms_msl_atanf;
using ::sms_msl_acosf; using ::sms_msl_sin; using ::sms_msl_cos; using ::sms_msl_atan2;
inline float sms_msl_atan2f(float y, float x) { return (float)::sms_msl_atan2((double)y, (double)x); }
using ::sms_msl_expf; using ::sms_msl_powf; using ::sms_msl_fmodf; using ::sms_msl_sqrtf;
}
#define sin(x) sms_msl_sin(x)
#define cos(x) sms_msl_cos(x)
#endif
#define sinf(x) sms_msl_sinf(x)
#define cosf(x) sms_msl_cosf(x)
#define tanf(x) sms_msl_tanf(x)
#define atanf(x) sms_msl_atanf(x)
#define atan2f(y, x) sms_msl_atan2f(y, x)
#define acosf(x) sms_msl_acosf(x)
#define atan2(y, x) sms_msl_atan2(y, x)
#define expf(x) sms_msl_expf(x)
#define powf(x, y) sms_msl_powf(x, y)
#define fmodf(x, y) sms_msl_fmodf(x, y)
#define sqrtf(x) sms_msl_sqrtf(x)

/* Heaps the game sizes with fixed GameCube constants (decomp-patches/ptr64-*):
 * with 8-byte pointers objects are up to twice as large, so 64-bit hosts
 * double them; 32-bit hosts keep retail's sizes. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define PORT_HEAP64(n) ((n) * 2)
#else
#define PORT_HEAP64(n) (n)
#endif

/* Endianness: game data on disc is big-endian. Loaders that the port has
 * patched call these (decomp-patches/). */
static inline u16 port_bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u32 port_bswap32(u32 v) { return __builtin_bswap32(v); }
static inline u32 port_be32(const void* p)
{
	const u8* b = (const u8*)p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline u16 port_be16(const void* p)
{
	const u8* b = (const u8*)p;
	return (u16)((b[0] << 8) | b[1]);
}
#ifdef __cplusplus
extern "C" {
#endif
/* Convert a whole RARC image (header, info block, nodes, file entries) to
 * native byte order in place. Idempotent: checks the magic's byte order. */
void port_rarc_to_native(void* arc);
/* Convert just the 0x20-byte RARC header / the info block and what follows
 * it, for loaders that read the pieces separately. */
void port_rarc_header_to_native(void* hdr);
void port_rarc_info_to_native(void* info);
/* Convert a resource file (recognised by its magic) to native byte order in
 * place; unknown formats are logged once and left alone. Idempotent. */
void port_res_to_native(void* data, u32 size);
void port_res_to_native_named(void* data, u32 size, const char* name);
/* SMS_NO_AUDIO: an empty init-data stream for JAudio, or NULL when audio is on. */
u8* port_noaudio_init_data(void);
extern int port_no_audio; /* SMS_NO_AUDIO=1 */
#ifdef __cplusplus
}
#endif

#endif /* SMS_PORT_COMPAT_H */
