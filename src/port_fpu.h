/* Gekko reciprocal / reciprocal-square-root estimates (frsqrte, fres),
 * bit-exact on every measured input.
 *
 * Source: black-box measurement, not code.
 * - tools/fpprobe/ assembles a DOL that runs frsqrte over every combination of
 *   exponent parity and the top 15 mantissa bits (65536 inputs), and fres over
 *   the top 15 mantissa bits (32768 inputs), plus 4096 random inputs.
 * - The DOL runs in the dolphin-oracle emulator; its interpreter and JIT agree.
 * - The measured results are piecewise linear in those mantissa bits, in 32
 *   segments each. tools/fpprobe/fit.py derives the base and decrement tables
 *   below, and tools/fpprobe/fpu_test.c checks this code against every measured
 *   value.
 * - Documentation: the PowerPC 750CL User's Manual (IBM) describes both
 *   instructions as table-based estimates accurate to 1/4096 (frsqrte) and
 *   1/4000 (fres). It does not give the tables; they are the measurement.
 *
 *   frsqrte: x = 1.m * 2^E, p = E & 1, q = floor(E / 2), i = top 15 bits of m,
 *            seg = 16p + i / 2048, j = i mod 2048,
 *            result mantissa = (base - dec * j) << 26, exponent 2^(-1 - q)
 *   fres:    seg = i / 1024, j = i mod 1024,
 *            result mantissa = (base - ((dec * j + 1) >> 1)) << 29,
 *            exponent 2^(-1 - E), sign of x; a single-precision value
 * Special values: +-0 -> +-inf; x < 0 -> NaN (frsqrte); +-inf -> +-0 (fres),
 * +inf -> +0 (frsqrte); NaN -> NaN.
 */
#ifndef SMS_PORT_FPU_H
#define SMS_PORT_FPU_H

static const unsigned int port_frsqrte_tab[32][2] = {
	{ 67084288, 1956 }, { 63082496, 1792 }, { 59416576, 1648 }, { 56041472, 1522 },
	{ 52924416, 1412 }, { 50032640, 1316 }, { 47341568, 1228 }, { 44826624, 1150 },
	{ 42471424, 1082 }, { 40259584, 1018 }, { 38174720, 962 },  { 36204544, 910 },
	{ 34344960, 862 },  { 32579584, 818 },  { 30904320, 778 },  { 29310976, 742 },
	{ 27781120, 1384 }, { 24950784, 1267 }, { 22358016, 1165 }, { 19972096, 1077 },
	{ 17768448, 999 },  { 15724544, 930 },  { 13819904, 869 },  { 12042240, 814 },
	{ 10375168, 764 },  { 8810496, 720 },   { 7335936, 680 },   { 5945344, 643 },
	{ 4628480, 609 },   { 3381248, 579 },   { 2197504, 550 },   { 1071104, 523 },
};

static const unsigned int port_fres_tab[32][2] = {
	{ 8386560, 993 }, { 7878656, 935 }, { 7399936, 881 }, { 6948864, 832 },
	{ 6522880, 787 }, { 6119936, 746 }, { 5738496, 708 }, { 5376000, 672 },
	{ 5031936, 639 }, { 4704768, 609 }, { 4392960, 581 }, { 4096000, 554 },
	{ 3812352, 530 }, { 3540992, 507 }, { 3281920, 485 }, { 3033600, 465 },
	{ 2795520, 446 }, { 2567168, 428 }, { 2348544, 411 }, { 2137600, 395 },
	{ 1935360, 380 }, { 1740800, 366 }, { 1551872, 347 }, { 1374208, 347 },
	{ 1197056, 323 }, { 1031680, 323 }, { 866304, 301 },  { 712192, 301 },
	{ 558080, 282 },  { 413696, 282 },  { 269312, 264 },  { 134144, 262 },
};

static inline unsigned long long port_f64_bits(double x)
{
	unsigned long long b;
	__builtin_memcpy(&b, &x, 8);
	return b;
}

static inline double port_f64_from_bits(unsigned long long b)
{
	double x;
	__builtin_memcpy(&x, &b, 8);
	return x;
}

/* Split a finite non-zero double into its unbiased exponent and 52-bit
 * mantissa (denormals normalised). */
static inline void port_f64_split(unsigned long long b, int* e, unsigned long long* m)
{
	int ex                = (int)((b >> 52) & 0x7FF);
	unsigned long long mm = b & 0xFFFFFFFFFFFFFULL;
	if (ex == 0) {
		ex = 1;
		while (!(mm & 0x10000000000000ULL)) {
			mm <<= 1;
			ex--;
		}
		mm &= 0xFFFFFFFFFFFFFULL;
	}
	*e = ex - 1023;
	*m = mm;
}

static inline double port_gekko_frsqrte(double x)
{
	unsigned long long b = port_f64_bits(x), m, mant;
	int e, q;
	unsigned int i, seg, j;
	if ((b & 0x7FFFFFFFFFFFFFFFULL) == 0)
		return port_f64_from_bits((b & 0x8000000000000000ULL) | 0x7FF0000000000000ULL);
	if (((b >> 52) & 0x7FF) == 0x7FF) {
		if (b & 0xFFFFFFFFFFFFFULL)
			return port_f64_from_bits(b | 0x0008000000000000ULL); /* NaN, quieted */
		return (b >> 63) ? port_f64_from_bits(0x7FF8000000000000ULL) : 0.0;
	}
	if (b >> 63)
		return port_f64_from_bits(0x7FF8000000000000ULL);
	port_f64_split(b, &e, &m);
	q    = (e - (e & 1)) / 2; /* floor(e / 2) */
	i    = (unsigned int)(m >> 37) & 0x7FFF;
	seg  = (unsigned int)(e & 1) * 16 + i / 2048;
	j    = i % 2048;
	mant = (unsigned long long)(port_frsqrte_tab[seg][0] - port_frsqrte_tab[seg][1] * j) << 26;
	return port_f64_from_bits(((unsigned long long)(1022 - q) << 52) | mant);
}

static inline float port_gekko_fres(double x)
{
	unsigned long long b = port_f64_bits(x), m, mant, sign = b & 0x8000000000000000ULL;
	int e, re;
	unsigned int i, seg, j;
	if ((b & 0x7FFFFFFFFFFFFFFFULL) == 0)
		return (float)port_f64_from_bits(sign | 0x7FF0000000000000ULL);
	if (((b >> 52) & 0x7FF) == 0x7FF) {
		if (b & 0xFFFFFFFFFFFFFULL)
			return (float)port_f64_from_bits(b | 0x0008000000000000ULL);
		return (float)port_f64_from_bits(sign);
	}
	port_f64_split(b, &e, &m);
	i   = (unsigned int)(m >> 37) & 0x7FFF;
	seg = i / 1024;
	j   = i % 1024;
	re  = 1022 - e;
	if (re <= 0 || re >= 2047) /* far outside single range: not measured */
		return (float)(1.0 / x);
	mant = (unsigned long long)(port_fres_tab[seg][0] - ((port_fres_tab[seg][1] * j + 1) >> 1)) << 29;
	return (float)port_f64_from_bits(sign | ((unsigned long long)re << 52) | mant);
}

/* MWCC's float-to-integer runtime conversions (runtime.c), which the game
 * reaches for every float or double it converts to a 32-bit unsigned integer
 * or to a 64-bit integer. The host's own conversions differ outside the
 * target range, and between the 32 and 64-bit builds (x86 wraps a negative
 * value; from 2^31 up the 32-bit build gives 0 or 0x80000000 and the 64-bit
 * build the low 32 bits); in range both are the same truncation.
 *
 * __cvt_fp2unsigned: 0 below zero and for NaN (fctiwz of NaN is 0x80000000,
 * less the 2^31 it adds back), 0xFFFFFFFF from 2^32 up (+inf included),
 * otherwise truncation towards zero. */
static inline unsigned int port_cvt_fp2unsigned(double d)
{
	if (!(d >= 0.0))
		return 0;
	if (d >= 4294967296.0)
		return 0xFFFFFFFFu;
	return (unsigned int)d;
}

/* __cvt_dbl_usll, which MWCC calls for (u64) and (s64) alike: truncation
 * towards zero as a signed 64-bit value (so a negative value wraps as a u64),
 * and from 2^63 in magnitude, infinities and NaN included, 0x7FFF...F or
 * 0x8000...0 by the sign bit. */
static inline unsigned long long port_cvt_dbl_usll(double d)
{
	if (d > -9223372036854775808.0 && d < 9223372036854775808.0)
		return (unsigned long long)(long long)d;
	return __builtin_signbit(d) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
}

/* The Gekko's quantised store: psq_st (and psq_stx, psq_stu, psq_stux) of a
 * paired-single register through a GQR whose store type is an integer
 * (GQRn bits 0-2: 4 u8, 5 u16, 6 s8, 7 s16; bits 8-13: a signed scale).
 * The value, taken as a single, is multiplied by 2^scale in single precision,
 * saturated to the type's range and converted towards zero; the store writes
 * that integer's low bytes. +-inf saturate; a NaN stores 0.
 *
 * Source: the GQR layout is the PowerPC 750CL (Gekko-compatible) User's
 * Manual's; the conversion is Dolphin's interpreter (ScaleAndClamp in
 * Interpreter_LoadStorePaired.cpp: scale, clamp, C conversion towards zero, a
 * NaN giving 0 on x86). It is not measured on hardware: there is neither a
 * console nor Dolphin here. The game's one integer quantised store outside the
 * THP decoder is OSf32tos8 in JASTrack (GQR4: s8, scale 0; docs/64-BIT.md,
 * item 18). */
static inline int port_gekko_quantize(double ps, unsigned int type, unsigned int scale)
{
	int e = (int)(scale & 63) - ((scale & 32) ? 64 : 0);
	float v = (float)ps * (float)__builtin_ldexp(1.0, e);
	float lo, hi;
	switch (type & 7) {
	case 4: lo = 0.0f; hi = 255.0f; break;
	case 5: lo = 0.0f; hi = 65535.0f; break;
	case 6: lo = -128.0f; hi = 127.0f; break;
	case 7: lo = -32768.0f; hi = 32767.0f; break;
	default: return 0; /* a float store: not a quantisation */
	}
	if (v != v)
		return 0;
	if (v < lo)
		v = lo;
	else if (v > hi)
		v = hi;
	return (int)v;
}

#endif
