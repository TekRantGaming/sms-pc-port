/* The Gekko's single-precision fused multiply-add (fmadds, ps_madd, ...):
 * a * c + b rounded once, to single, as the PowerPC architecture defines it.
 * Shared by platform/mtx (the MTX/VEC library and JSystem's paired-single
 * routines) and platform/misc/msl_math.c (MSL's trigonometry).
 *
 * port_fmas_soft needs no FMA instruction. The product of two floats is exact
 * in double, so only the sum rounds twice (to double, then to single), and
 * that differs from rounding once only when the double sum lands exactly
 * halfway between two floats (or in the float subnormal range, where the
 * halfway points are coarser): then the sum is redone with its exact error
 * (TwoSum) and rounded to odd, which the conversion to float then rounds
 * correctly. It is identical to glibc's fmaf on 200,000,000 random triples in
 * each word size, and to the CPU's FMA instruction (tools/mtxmath).
 *
 * PORT_OPAQUE hides a value from the optimiser: the Gekko's fnmadds and
 * fnmsubs negate the rounded result, -(a * c + b) and -(a * c - b), and a
 * compiler folds a negated fma into the x86 fnmadd/fnmsub forms, -(a * c) + b
 * and -(a * c) - b, which differ in the sign of a zero result. */
#ifndef SMS_PORT_FMA_H
#define SMS_PORT_FMA_H

#include <string.h>

static inline float port_fmas_soft(float a, float c, float b)
{
	double p = (double)a * c, s = p + b;
	unsigned long long u;
	memcpy(&u, &s, 8);
	if (__builtin_expect((u & 0x1FFFFFFFu) == 0x10000000u || __builtin_fabs(s) < 0x1p-126, 0)) {
		double bb = s - p, e = (p - (s - bb)) + (b - bb);
		if (e != 0.0 && e == e && !(u & 1)) {
			u += (e > 0) == (s > 0) ? 1 : -1;
			memcpy(&s, &u, 8);
		}
	}
	return (float)s;
}

#if defined(__x86_64__) || defined(__i386__)
#define PORT_OPAQUE(x) __asm__("" : "+x"(x))
#elif defined(__aarch64__)
#define PORT_OPAQUE(x) __asm__("" : "+w"(x))
#else
#define PORT_OPAQUE(x) __asm__("" : "+m"(x))
#endif

#endif
