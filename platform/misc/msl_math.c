/* The GameCube's MSL maths, for the host.
 *
 * The game calls the maths library that shipped in its DOL, not the host's:
 *   MSL_C.PPCEABI.bare.H.a  trigf.c         sinf, cosf, tanf
 *                           inverse_trig.c  atanf, atan2f, acosf, _inv_sqrtf
 *                           exponentialsf.c expf, powf
 *                           s_atan.c        atan    (fdlibm)
 *                           e_atan2.c       __ieee754_atan2 (fdlibm)
 *                           w_atan2.c       atan2   (the fdlibm wrapper)
 * and two of MSL's header inlines, std::fmodf (the DOL's weak copy in
 * wireTrap.cpp, which calls the runtime's __cvt_dbl_usll and __cvt_sll_flt)
 * and std::sqrtf (frsqrte and three Newton-Raphson steps, expanded at most
 * call sites; see sms_msl_sqrtf).
 * The decomp (decomp/libs/PowerPC_EABI_Support/src/Msl/MSL_C/
 * MSL_Common_Embedded/Math) has every one of them byte-matching, and the port
 * routes the game's calls here (src/port_compat.h) so both word sizes, and
 * every host libm, compute what the console computed.
 *
 * The C sources cannot simply be compiled for the host: MWCC turned their
 * polynomials into Gekko fused multiply-adds (fmadds, fnmsubs, fmadd, ...),
 * whose roundings a host compiler does not reproduce. So each function below
 * follows the matched object's instructions one for one (the comments name
 * them), with these host equivalents:
 *
 *   fadds/fsubs/fmuls/fdivs  float arithmetic, one rounding to single. This
 *                            file is built with SSE maths on x86
 *                            (CMakeLists.txt), never x87 extended precision.
 *   fmadds a,c,b             a * c + b rounded once, to single, as the PowerPC
 *                            architecture defines it (src/port_fma.h).
 *   fnmsubs / fnmadds        the same, the rounded result negated.
 *   fmadd/fmsub/fnmsub       fma(), fused in double (glibc's is exact).
 *   fctiwz                   truncation that saturates like the Gekko's.
 *   xoris/0x43300000/fsubs   MWCC's int to float conversion: the exact double
 *                            rounded to single, i.e. (float)n.
 *   frsqrte                  port_gekko_frsqrte (src/port_fpu.h), the
 *                            measured hardware estimate.
 * Constants are the DOL's bit patterns (.rodata/.sdata2 of the objects
 * above), not decimal literals.
 *
 * tools/mslmath/ checks this file against the original machine code run under
 * qemu-ppc, and the 32-bit build against the 64-bit build. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "port_fpu.h"
#include "port_fma.h"
#include "msl_math.h"

typedef union {
	uint32_t u;
	float f;
} fbits_t;
typedef union {
	uint64_t u;
	double d;
} dbits_t;

static inline uint32_t f2u(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}
static inline float u2f(uint32_t u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static inline uint64_t d2u(double d)
{
	uint64_t u;
	memcpy(&u, &d, 8);
	return u;
}
static inline double u2d(uint64_t u)
{
	double d;
	memcpy(&d, &u, 8);
	return d;
}

/* Gekko single-precision fused multiply-adds, rounded once (src/port_fma.h):
 * port_fmas_soft on x86 hosts, whose CPUs may lack an FMA instruction, and
 * fmaf elsewhere (an instruction on arm64; tools/mslmath checks that path on
 * x86 with MSL_MATH_TEST_FMA and -mfma). The negated forms negate the
 * rounded result, as the Gekko does: fnmsubs gives -0 where a * c == b. */
#if (defined(__x86_64__) || defined(__i386__)) && !defined(MSL_MATH_TEST_FMA)
#define FMAS(a, c, b) port_fmas_soft(a, c, b)
#else
#define FMAS(a, c, b) fmaf(a, c, b)
#endif
static inline float fmadds(float a, float c, float b) { return FMAS(a, c, b); }
static inline float fnmadds(float a, float c, float b)
{
	float r = FMAS(a, c, b);
	PORT_OPAQUE(r);
	return -r;
}
static inline float fnmsubs(float a, float c, float b)
{
	float r = FMAS(a, c, -b);
	PORT_OPAQUE(r);
	return -r;
}

/* The hottest functions here (sinf, cosf, expf, powf; the game calls them
 * thousands of times a frame) are built twice on x86, once more with the FMA
 * instruction (whose single rounding is what port_fmas_soft computes), and
 * each call takes that build where the CPU has the instruction:
 * msl_math_hot.h includes msl_math_trig.inc and msl_math_pow.inc for it. */
#if (defined(__x86_64__) || defined(__i386__)) && !defined(MSL_MATH_TEST_FMA) && defined(__GNUC__)
#define MSL_FMA_DISPATCH 1
#include <cpuid.h>
#define MSL_FMA_TARGET __attribute__((target("fma")))
static inline MSL_FMA_TARGET float hw_fmadds(float a, float c, float b) { return __builtin_fmaf(a, c, b); }
static inline MSL_FMA_TARGET float hw_fnmadds(float a, float c, float b)
{
	float r = __builtin_fmaf(a, c, b);
	PORT_OPAQUE(r);
	return -r;
}
static inline MSL_FMA_TARGET float hw_fnmsubs(float a, float c, float b)
{
	float r = __builtin_fmaf(a, c, -b);
	PORT_OPAQUE(r);
	return -r;
}
/* FMA, and the OS saving the AVX registers it uses. */
static int msl_have_fma(void)
{
	static int have = -1;
	if (have < 0) {
		unsigned a, b, c, d, lo, hi;
		have = 0;
		if (__get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 12)) && (c & (1u << 27)) && (c & (1u << 28))) {
			__asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
			have = (lo & 6) == 6;
		}
	}
	return have;
}
#else
#define MSL_FMA_DISPATCH 0
#endif

#ifdef MSL_MATH_TEST_FRSQRTE
double MSL_MATH_TEST_FRSQRTE(double);
#define frsqrte MSL_MATH_TEST_FRSQRTE
#else
#define frsqrte port_gekko_frsqrte
#endif

/* fctiwz: toward zero; NaN and values below -2^31 give 0x80000000, values
 * from 2^31 up give 0x7fffffff. */
static inline int32_t fctiwz(float v)
{
	if (v != v || v < -2147483648.0f)
		return INT32_MIN;
	if (v >= 2147483648.0f)
		return INT32_MAX;
	return (int32_t)v;
}

/* ---- trigf.c (0x8033C5CC) and common_float_tables.c --------------------- */

static const fbits_t four_over_pi_m1[4] = { /* tmp_float, copied by __sinit_trigf_c */
	{ 0x3e800000 }, { 0x3cbe6080 }, { 0x34372200 }, { 0x2da44152 },
};
static const fbits_t sincos_on_quadrant[8] = {
	{ 0x00000000 }, { 0x3f800000 }, { 0x3f800000 }, { 0x00000000 },
	{ 0x00000000 }, { 0xbf800000 }, { 0xbf800000 }, { 0x00000000 },
};
static const fbits_t sincos_poly[10] = {
	{ 0x366ccfaa }, { 0x34a5e129 }, { 0xb9aae275 }, { 0xb8196543 }, { 0x3c81e0ed },
	{ 0x3b2335dd }, { 0xbe9de9e6 }, { 0xbda55de7 }, { 0x3f800000 }, { 0x3f490fdb },
};
#define P(i) sincos_poly[i].f
#define Q(i) sincos_on_quadrant[i].f
static const fbits_t two_over_pi = { 0x3f22f983 }; /* @106 */
static const fbits_t half        = { 0x3f000000 }; /* @107 */
static const fbits_t sqrt_eps    = { 0x39b504f3 }; /* @108 */

#define MSL_HOT_PART "msl_math_trig.inc"
#include "msl_math_hot.h"
#undef MSL_HOT_PART
#if MSL_FMA_DISPATCH
float sms_msl_cosf(float x) { return msl_have_fma() ? sms_msl_cosf_hw(x) : sms_msl_cosf_soft(x); }
float sms_msl_sinf(float x) { return msl_have_fma() ? sms_msl_sinf_hw(x) : sms_msl_sinf_soft(x); }
#endif

/* tanf calls cos(float), then sin(float), and divides. */
float sms_msl_tanf(float x)
{
	float c = sms_msl_cosf(x);
	float s = sms_msl_sinf(x);
	return s / c; /* fdivs */
}

/* ---- inverse_trig.c (0x8033C22C) --------------------------------------- */

static const fbits_t atan_coeff[7] = {
	{ 0x3f800000 }, { 0xbeaaaaaa }, { 0x3e4ccc81 }, { 0xbe123e7d },
	{ 0x3de21f95 }, { 0xbdad417c }, { 0x3d41186d },
};
static const fbits_t onep_one_over_xisqr_hi[6] = {
	{ 0x40da826b }, { 0x404f5958 }, { 0x40000000 }, { 0x3fb925ab }, { 0x3f95f61a }, { 0x3f851081 },
};
static const fbits_t onep_one_over_xisqr_lo[6] = {
	{ 0x36ef692f }, { 0x355c1df9 }, { 0x00000000 }, { 0x35291d45 }, { 0x00000000 }, { 0x00000000 },
};
static const fbits_t atan_xi_hi[7] = {
	{ 0x00000000 }, { 0x3ec90eaa }, { 0x3f16cbe4 }, { 0x3f490fda },
	{ 0x3f7b53c5 }, { 0x3f96cbe2 }, { 0x3fafedd9 },
};
static const fbits_t atan_xi_lo[7] = {
	{ 0x00000000 }, { 0x37185d99 }, { 0x32c59189 }, { 0x33874a9e },
	{ 0x353cfa83 }, { 0x348637bd }, { 0x35541063 },
};
static const fbits_t one_over_xi_hi[6] = {
	{ 0x401a8277 }, { 0x3fbf90c7 }, { 0x3f800000 }, { 0x3f2b0dc1 }, { 0x3ed413cd }, { 0x3e4bafaf },
};
static const fbits_t one_over_xi_lo[6] = {
	{ 0x3516dc59 }, { 0x00000000 }, { 0x00000000 }, { 0x00000000 }, { 0x00000000 }, { 0x00000000 },
};
static const fbits_t tan_3pi_8  = { 0x401a827a }; /* @156, 2.4142137 */
static const fbits_t tan_pi_8   = { 0x3ed413cd }; /* @158, 0.41421357 */
static const fbits_t half_pi    = { 0x3fc90fdb }; /* @159 */
static const fbits_t pi_f       = { 0x40490fdb }; /* @188 */
static const fbits_t float_nan  = { 0x7fffffff }; /* __float_nan */
static const fbits_t float_huge = { 0x7f800000 }; /* __float_huge */

float sms_msl_atanf(float x)
{
	const uint32_t sign = f2u(x) & 0x80000000;
	const float ax      = u2f(f2u(x) & 0x7fffffff);
	int index = -1, inv = 0;
	float z, zsq, z3, p;

	if (ax >= tan_3pi_8.f) {
		z   = 1.0f / ax;
		inv = 1;
	} else if (tan_pi_8.f < ax) {
		const int32_t b = (int32_t)f2u(ax);
		float hi, lo, a, c;
		index = 0;
		switch (b & 0x7f800000) {
		case 0x3f000000: /* .5 <= x < 1 */
			if (b >= 0x3f08d5b9)
				index = 1;
			if (b >= 0x3f521801)
				index++;
			break;
		case 0x3f800000: /* 1 <= x < 2 */
			index = b >= 0x3f9bf7ec ? 3 : 2;
			if (b >= 0x3fef789e)
				index++;
			break;
		case 0x40000000: /* 2 <= x < 2.414213565f */
			index = 4;
			break;
		}
		hi = one_over_xi_hi[index].f;
		lo = one_over_xi_lo[index].f;
		z  = 1.0f / (hi + (ax + lo));
		a  = fnmsubs(z, onep_one_over_xisqr_hi[index].f, hi);
		c  = fnmsubs(z, onep_one_over_xisqr_lo[index].f, lo);
		z  = a + c;
	} else {
		z = ax;
	}

	zsq = z * z;
	p   = fmadds(zsq, atan_coeff[6].f, atan_coeff[5].f);
	z3  = z * zsq;
	p   = fmadds(zsq, p, atan_coeff[4].f);
	p   = fmadds(zsq, p, atan_coeff[3].f);
	p   = fmadds(zsq, p, atan_coeff[2].f);
	p   = fmadds(zsq, p, atan_coeff[1].f);
	z   = fmadds(z3, p, z);
	z   = z + atan_xi_lo[index + 1].f;
	z   = z + atan_xi_hi[index + 1].f;

	if (inv) {
		z = z - half_pi.f;
		return sign ? z : -z;
	}
	return u2f(f2u(z) | sign);
}

/* _inv_sqrtf: the frsqrte estimate (rounded to single by frsp) and three
 * Newton-Raphson steps in single precision. */
static float msl_inv_sqrtf(float x)
{
	if (x > 0.0f) {
		float g = (float)frsqrte((double)x), t;
		int i;
		for (i = 0; i < 3; i++) {
			t = g * g;
			g = half.f * g;
			t = fnmsubs(x, t, 3.0f);
			g = g * t;
		}
		return g;
	}
	if (x == 0.0f)
		return float_huge.f;
	return float_nan.f;
}

#ifdef MSL_MATH_TEST_HOOKS
float msl_inv_sqrtf_for_test(float x) { return msl_inv_sqrtf(x); }
#endif

float sms_msl_acosf(float x)
{
	float r = msl_inv_sqrtf(fnmsubs(x, x, 1.0f));
	return half_pi.f - sms_msl_atanf(x * r);
}

float sms_msl_atan2f(float y, float x)
{
	const uint32_t sy = f2u(y) & 0x80000000;
	const uint32_t sx = f2u(x) & 0x80000000;
	if (sx == sy) {
		if (sx)
			return sms_msl_atanf(y / x) - pi_f.f;
		if (x == 0.0f)
			return half_pi.f;
		return sms_msl_atanf(y / x);
	}
	if (x < 0.0f)
		return pi_f.f + sms_msl_atanf(y / x);
	if (x != 0.0f)
		return sms_msl_atanf(y / x);
	return u2f(sy + 0x3fc90fdb); /* +-pi/2 */
}

/* ---- s_atan.c (0x8033BF28), fdlibm ------------------------------------- */

static const dbits_t atanhi[4] = {
	{ 0x3fddac670561bb4fULL }, { 0x3fe921fb54442d18ULL },
	{ 0x3fef730bd281f69bULL }, { 0x3ff921fb54442d18ULL },
};
static const dbits_t atanlo[4] = {
	{ 0x3c7a2b7f222f65e2ULL }, { 0x3c81a62633145c07ULL },
	{ 0x3c7007887af0cbbdULL }, { 0x3c91a62633145c07ULL },
};
static const dbits_t aT[11] = {
	{ 0x3fd555555555550dULL }, { 0xbfc999999998ebc4ULL }, { 0x3fc24924920083ffULL },
	{ 0xbfbc71c6fe231671ULL }, { 0x3fb745cdc54c206eULL }, { 0xbfb3b0f2af749a6dULL },
	{ 0x3fb10d66a0d03d51ULL }, { 0xbfadde2d52defd9aULL }, { 0x3fa97b4b24760debULL },
	{ 0xbfa2b4442c6a6c2fULL }, { 0x3f90ad3ae322da11ULL },
};
static const dbits_t huge_d = { 0x7e37e43c8800759cULL }; /* 1e300 */

double sms_msl_atan(double x)
{
	const int32_t hx = (int32_t)(d2u(x) >> 32);
	const int32_t ix = hx & 0x7fffffff;
	double z, w, s1, s2, t0, t1, t2, t3, t4;
	int id;

	if (ix >= 0x44100000) { /* |x| >= 2^66 */
		if (ix > 0x7ff00000 || (ix == 0x7ff00000 && (uint32_t)d2u(x) != 0))
			return x + x; /* NaN */
		if (hx > 0)
			return atanhi[3].d + atanlo[3].d;
		return -atanhi[3].d - atanlo[3].d;
	}
	if (ix < 0x3fdc0000) { /* |x| < 0.4375 */
		if (ix < 0x3e200000 && huge_d.d + x > 1.0)
			return x;
		id = -1;
	} else {
		x = fabs(x);
		if (ix < 0x3ff30000) {
			if (ix < 0x3fe60000) { /* 7/16 <= |x| < 11/16 */
				id = 0;
				x  = fma(2.0, x, -1.0) / (2.0 + x);
			} else { /* 11/16 <= |x| < 19/16 */
				id = 1;
				x  = (x - 1.0) / (1.0 + x);
			}
		} else {
			if (ix < 0x40038000) { /* |x| < 2.4375 */
				id = 2;
				x  = (x - 1.5) / fma(1.5, x, 1.0);
			} else { /* 2.4375 <= |x| < 2^66 */
				id = 3;
				x  = -1.0 / x;
			}
		}
	}
	z  = x * x;
	w  = z * z;
	t0 = fma(w, aT[10].d, aT[8].d);
	t1 = fma(w, aT[9].d, aT[7].d);
	t2 = fma(w, t0, aT[6].d);
	t3 = fma(w, t1, aT[5].d);
	t4 = fma(w, t2, aT[4].d);
	t1 = fma(w, t3, aT[3].d);
	t2 = fma(w, t4, aT[2].d);
	t3 = fma(w, t1, aT[1].d);
	t4 = fma(w, t2, aT[0].d);
	s2 = w * t3;
	s1 = z * t4;
	if (id < 0)
		return fma(-x, s1 + s2, x); /* fnmsub */
	z = atanhi[id].d - (fma(x, s1 + s2, -atanlo[id].d) - x);
	return hx < 0 ? -z : z;
}

/* ---- e_atan2.c (0x8033BC90) and w_atan2.c, fdlibm ---------------------- */

static const dbits_t pi_d      = { 0x400921fb54442d18ULL };
static const dbits_t pi_o_2    = { 0x3ff921fb54442d18ULL };
static const dbits_t pi_o_4    = { 0x3fe921fb54442d18ULL };
static const dbits_t pi3_o_4   = { 0x4002d97c7f3321d2ULL };
static const dbits_t pi_lo     = { 0x3ca1a62633145c07ULL };

double sms_msl_atan2(double y, double x)
{
	const uint64_t bx = d2u(x), by = d2u(y);
	const int32_t hx = (int32_t)(bx >> 32), hy = (int32_t)(by >> 32);
	const uint32_t lx = (uint32_t)bx, ly = (uint32_t)by;
	const uint32_t ix = (uint32_t)hx & 0x7fffffff, iy = (uint32_t)hy & 0x7fffffff;
	int m, k;
	double z;

	if ((ix | ((lx | (0u - lx)) >> 31)) > 0x7ff00000u || (iy | ((ly | (0u - ly)) >> 31)) > 0x7ff00000u)
		return x + y; /* NaN */
	if ((((uint32_t)hx - 0x3ff00000u) | lx) == 0)
		return sms_msl_atan(y); /* x = 1.0 */
	m = ((hy >> 31) & 1) | ((hx >> 30) & 2);

	if ((iy | ly) == 0) { /* y = 0 */
		switch (m) {
		case 0:
		case 1: return y;
		case 2: return pi_d.d;
		case 3: return -pi_d.d;
		}
	}
	if ((ix | lx) == 0) /* x = 0 */
		return hy < 0 ? -pi_o_2.d : pi_o_2.d;
	if (ix == 0x7ff00000) { /* x = inf */
		if (iy == 0x7ff00000) {
			switch (m) {
			case 0: return pi_o_4.d;
			case 1: return -pi_o_4.d;
			case 2: return pi3_o_4.d;
			case 3: return -pi3_o_4.d;
			}
		} else {
			switch (m) {
			case 0: return 0.0;
			case 1: return -0.0;
			case 2: return pi_d.d;
			case 3: return -pi_d.d;
			}
		}
	}
	if (iy == 0x7ff00000) /* y = inf */
		return hy < 0 ? -pi_o_2.d : pi_o_2.d;

	k = ((int32_t)iy - (int32_t)ix) >> 20;
	if (k > 60)
		z = pi_o_2.d;
	else if (hx < 0 && k < -60)
		z = 0.0;
	else
		z = sms_msl_atan(fabs(y / x));
	switch (m) {
	case 0: return z;
	case 1: return u2d(d2u(z) ^ 0x8000000000000000ULL);
	case 2: return pi_d.d - (z - pi_lo.d);
	default: return (z - pi_lo.d) - pi_d.d;
	}
}


/* ---- exponentialsf.c (0x8033C9B8) and common_float_tables.c ----------- */

/* __log2_F, __two_to_x and expf's __exp_to_x (exponentialsf.c .rodata),
 * __one_over_F and __two_to_log2e_m1_tI (common_float_tables.c). */
static const fbits_t log2_F[129] = {
	{ 0xbec00000 }, { 0xbeba406c }, { 0xbeb48c35 }, { 0xbeaee32e }, { 0xbea9452d }, { 0xbea3b205 },
	{ 0xbe9e298f }, { 0xbe98aba0 }, { 0xbe933812 }, { 0xbe8dcebd }, { 0xbe886f7b }, { 0xbe831a28 },
	{ 0xbe7b9d3c }, { 0xbe711973 }, { 0xbe66a8b1 }, { 0xbe5c4ab0 }, { 0xbe51ff2e }, { 0xbe47c5e9 },
	{ 0xbe3d9ea1 }, { 0xbe338918 }, { 0xbe29850f }, { 0xbe1f924a }, { 0xbe15b08e }, { 0xbe0bdfa1 },
	{ 0xbe021f4a }, { 0xbdf0dea4 }, { 0xbddd9f05 }, { 0xbdca7f4a }, { 0xbdb77f0b }, { 0xbda49de0 },
	{ 0xbd91db66 }, { 0xbd7e6e71 }, { 0xbd5961ed }, { 0xbd349081 }, { 0xbd0ff971 }, { 0xbcd7380e },
	{ 0xbc8eef19 }, { 0xbc0e2d45 }, { 0x38256316 }, { 0x3c0e9c73 }, { 0x3c8ddd45 }, { 0x3cd4011d },
	{ 0x3d0cdd83 }, { 0x3d2f861e }, { 0x3d51fafe }, { 0x3d743cba }, { 0x3d8b25f6 }, { 0x3d9c1492 },
	{ 0x3dacea7c }, { 0x3dbda7fb }, { 0x3dce4d54 }, { 0x3ddedace }, { 0x3def50ad }, { 0x3dffaf33 },
	{ 0x3e07fb51 }, { 0x3e10139e }, { 0x3e1820a0 }, { 0x3e202276 }, { 0x3e28193f }, { 0x3e30051a },
	{ 0x3e37e624 }, { 0x3e3fbc7a }, { 0x3e47883a }, { 0x3e4f4981 }, { 0x3e570069 }, { 0x3e5ead0f },
	{ 0x3e664f8d }, { 0x3e6de7ff }, { 0x3e75767f }, { 0x3e7cfb27 }, { 0x3e823b08 }, { 0x3e85f3aa },
	{ 0x3e89a785 }, { 0x3e8d56a6 }, { 0x3e910118 }, { 0x3e94a6e9 }, { 0x3e984822 }, { 0x3e9be4d1 },
	{ 0x3e9f7cff }, { 0x3ea310b9 }, { 0x3ea6a009 }, { 0x3eaa2afa }, { 0x3eadb197 }, { 0x3eb133ea },
	{ 0x3eb4b1fd }, { 0x3eb82bdc }, { 0x3ebba190 }, { 0x3ebf1322 }, { 0x3ec2809d }, { 0x3ec5ea0b },
	{ 0x3ec94f75 }, { 0x3eccb0e4 }, { 0x3ed00e61 }, { 0x3ed367f7 }, { 0x3ed6bdad }, { 0x3eda0f8d },
	{ 0x3edd5da0 }, { 0x3ee0a7ee }, { 0x3ee3ee7f }, { 0x3ee7315d }, { 0x3eea708f }, { 0x3eedac1e },
	{ 0x3ef0e412 }, { 0x3ef41873 }, { 0x3ef74949 }, { 0x3efa769b }, { 0x3efda072 }, { 0x3f00636a },
	{ 0x3f01f4e5 }, { 0x3f0384ad }, { 0x3f0512c7 }, { 0x3f069f35 }, { 0x3f0829fb }, { 0x3f09b31e },
	{ 0x3f0b3a9f }, { 0x3f0cc083 }, { 0x3f0e44cd }, { 0x3f0fc781 }, { 0x3f1148a1 }, { 0x3f12c832 },
	{ 0x3f144636 }, { 0x3f15c2b0 }, { 0x3f173da4 }, { 0x3f18b714 }, { 0x3f1a2f04 }, { 0x3f1ba578 },
	{ 0x3f1d1a71 }, { 0x3f1e8df2 }, { 0x3f200000 },
};
static const fbits_t two_to_x[9] = {
	{ 0x3f317218 }, { 0x3e75fdf0 }, { 0x3d635854 }, { 0x3c1d9561 }, { 0x3aaebe2f }, { 0x3921805e },
	{ 0x3781e214 }, { 0x35b3c15f }, { 0x33dd30d7 },
};
static const fbits_t exp_to_x[8] = {
	{ 0x3f7ffffe }, { 0x3effffff }, { 0x3e2aab03 }, { 0x3d2aaae6 }, { 0x3c0874aa }, { 0x3ab5f6d0 },
	{ 0x3956a4b8 }, { 0x37d5e715 },
};
static const fbits_t one_over_F[129] = {
	{ 0x3f800000 }, { 0x3f7e03f8 }, { 0x3f7c0fc1 }, { 0x3f7a232d }, { 0x3f783e10 }, { 0x3f76603e },
	{ 0x3f74898d }, { 0x3f72b9d6 }, { 0x3f70f0f1 }, { 0x3f6f2eb7 }, { 0x3f6d7304 }, { 0x3f6bbdb3 },
	{ 0x3f6a0ea1 }, { 0x3f6865ac }, { 0x3f66c2b4 }, { 0x3f652598 }, { 0x3f638e39 }, { 0x3f61fc78 },
	{ 0x3f607038 }, { 0x3f5ee95c }, { 0x3f5d67c9 }, { 0x3f5beb62 }, { 0x3f5a740e }, { 0x3f5901b2 },
	{ 0x3f579436 }, { 0x3f562b81 }, { 0x3f54c77b }, { 0x3f53680d }, { 0x3f520d21 }, { 0x3f50b6a0 },
	{ 0x3f4f6475 }, { 0x3f4e168a }, { 0x3f4ccccd }, { 0x3f4b8728 }, { 0x3f4a4588 }, { 0x3f4907da },
	{ 0x3f47ce0c }, { 0x3f46980c }, { 0x3f4565c8 }, { 0x3f443730 }, { 0x3f430c31 }, { 0x3f41e4bc },
	{ 0x3f40c0c1 }, { 0x3f3fa030 }, { 0x3f3e82fa }, { 0x3f3d6910 }, { 0x3f3c5264 }, { 0x3f3b3ee7 },
	{ 0x3f3a2e8c }, { 0x3f392144 }, { 0x3f381703 }, { 0x3f370fbb }, { 0x3f360b61 }, { 0x3f3509e7 },
	{ 0x3f340b41 }, { 0x3f330f63 }, { 0x3f321643 }, { 0x3f311fd4 }, { 0x3f302c0b }, { 0x3f2f3ade },
	{ 0x3f2e4c41 }, { 0x3f2d602b }, { 0x3f2c7692 }, { 0x3f2b8f6a }, { 0x3f2aaaab }, { 0x3f29c84a },
	{ 0x3f28e83f }, { 0x3f280a81 }, { 0x3f272f05 }, { 0x3f2655c4 }, { 0x3f257eb5 }, { 0x3f24a9cf },
	{ 0x3f23d70a }, { 0x3f23065e }, { 0x3f2237c3 }, { 0x3f216b31 }, { 0x3f20a0a1 }, { 0x3f1fd80a },
	{ 0x3f1f1166 }, { 0x3f1e4cad }, { 0x3f1d89d9 }, { 0x3f1cc8e1 }, { 0x3f1c09c1 }, { 0x3f1b4c70 },
	{ 0x3f1a90e8 }, { 0x3f19d723 }, { 0x3f191f1a }, { 0x3f1868c8 }, { 0x3f17b426 }, { 0x3f17012e },
	{ 0x3f164fda }, { 0x3f15a025 }, { 0x3f14f209 }, { 0x3f144581 }, { 0x3f139a86 }, { 0x3f12f114 },
	{ 0x3f124925 }, { 0x3f11a2b4 }, { 0x3f10fdbc }, { 0x3f105a38 }, { 0x3f0fb824 }, { 0x3f0f177a },
	{ 0x3f0e7835 }, { 0x3f0dda52 }, { 0x3f0d3dcb }, { 0x3f0ca29c }, { 0x3f0c08c1 }, { 0x3f0b7034 },
	{ 0x3f0ad8f3 }, { 0x3f0a42f8 }, { 0x3f09ae41 }, { 0x3f091ac7 }, { 0x3f088889 }, { 0x3f07f781 },
	{ 0x3f0767ab }, { 0x3f06d905 }, { 0x3f064b8a }, { 0x3f05bf37 }, { 0x3f053408 }, { 0x3f04a9fa },
	{ 0x3f042108 }, { 0x3f039930 }, { 0x3f03126f }, { 0x3f028cc0 }, { 0x3f020821 }, { 0x3f01848e },
	{ 0x3f010204 }, { 0x3f008080 }, { 0x3f000000 },
};
static const fbits_t two_to_log2e_m1_tI[178] = {
	{ 0x2c03db89 }, { 0x2c333687 }, { 0x2c739362 }, { 0x2ca586e0 }, { 0x2ce0f96d }, { 0x2d18e2cb },
	{ 0x2d4fcb22 }, { 0x2d8d35d7 }, { 0x2dbfecba }, { 0x2e026d27 }, { 0x2e314490 }, { 0x2e70ee94 },
	{ 0x2ea3baf0 }, { 0x2ede884f }, { 0x2f1739fb }, { 0x2f4d89c1 }, { 0x2f8bad78 }, { 0x2fbdd771 },
	{ 0x300102bf }, { 0x302f5800 }, { 0x306e511e }, { 0x30a1f3fe }, { 0x30dc1df9 }, { 0x311595c7 },
	{ 0x314b4ea4 }, { 0x318a295c }, { 0x31bbc7f1 }, { 0x31ff388b }, { 0x322d70c9 }, { 0x326bbaec },
	{ 0x32a031fc }, { 0x32d9ba5a }, { 0x3313f623 }, { 0x334919b9 }, { 0x3388a975 }, { 0x33b9be2b },
	{ 0x33fc7361 }, { 0x342b8edc }, { 0x34692beb }, { 0x349e74dd }, { 0x34d75d5d }, { 0x35125b02 },
	{ 0x3546eaf1 }, { 0x35872dba }, { 0x35b7ba0f }, { 0x35f9b5ea }, { 0x3629b229 }, { 0x3666a405 },
	{ 0x369cbc92 }, { 0x36d506f2 }, { 0x3710c457 }, { 0x3744c239 }, { 0x3785b61d }, { 0x37b5bb8d },
	{ 0x37f7000f }, { 0x3827daa4 }, { 0x38642328 }, { 0x389b090f }, { 0x38d2b706 }, { 0x390f3216 },
	{ 0x39429f81 }, { 0x39844295 }, { 0x39b3c295 }, { 0x39f451bd }, { 0x3a26083d }, { 0x3a61a93f },
	{ 0x3a995a46 }, { 0x3ad06d87 }, { 0x3b0da433 }, { 0x3b4082b8 }, { 0x3b82d314 }, { 0x3bb1cf19 },
	{ 0x3bf1aade }, { 0x3c243ae5 }, { 0x3c5f3638 }, { 0x3c97b02a }, { 0x3cce2a62 }, { 0x3d0c1aa1 },
	{ 0x3d3e6bce }, { 0x3d816791 }, { 0x3dafe108 }, { 0x3def0b5d }, { 0x3e227290 }, { 0x3e5cc9ff },
	{ 0x3e960aae }, { 0x3ecbed86 }, { 0x3f0a9555 }, { 0x3f3c5ab2 }, { 0x3f800000 }, { 0x3fadf854 },
	{ 0x3fec7326 }, { 0x4020af2e }, { 0x405a6481 }, { 0x409469c5 }, { 0x40c9b6e3 }, { 0x41091443 },
	{ 0x413a4f54 }, { 0x417d38ac }, { 0x41ac14ee }, { 0x41e9e224 }, { 0x421ef0b3 }, { 0x425805ad },
	{ 0x4292cd62 }, { 0x42c78665 }, { 0x4307975f }, { 0x433849a4 }, { 0x437a7910 }, { 0x43aa36c8 },
	{ 0x43e75844 }, { 0x441d3710 }, { 0x4455ad6e }, { 0x4491357a }, { 0x44c55bfe }, { 0x45061e9d },
	{ 0x45364993 }, { 0x4577c118 }, { 0x45a85dd2 }, { 0x45e4d572 }, { 0x461b8238 }, { 0x46535bb3 },
	{ 0x468fa1fe }, { 0x46c3379a }, { 0x4704a9f1 }, { 0x47344f11 }, { 0x477510ad }, { 0x47a689fe },
	{ 0x47e2599a }, { 0x4819d21f }, { 0x4851106a }, { 0x488e12e4 }, { 0x48c1192b }, { 0x49033952 },
	{ 0x49325a0e }, { 0x497267bb }, { 0x49a4bb3e }, { 0x49dfe4a9 }, { 0x4a1826b5 }, { 0x4a4ecb81 },
	{ 0x4a8c881f }, { 0x4abf009e }, { 0x4b01ccb3 }, { 0x4b306a7c }, { 0x4b6fc62e }, { 0x4ba2f184 },
	{ 0x4bdd768b }, { 0x4c167ff0 }, { 0x4c4c8ce5 }, { 0x4c8b01a3 }, { 0x4cbcede5 }, { 0x4d006408 },
	{ 0x4d2e804a }, { 0x4d6d2bef }, { 0x4da12cc1 }, { 0x4ddb0f2e }, { 0x4e14ddc1 }, { 0x4e4a5487 },
	{ 0x4e897f64 }, { 0x4ebae0ee }, { 0x4efdfe91 }, { 0x4f2c9b6a }, { 0x4f6a98ec }, { 0x4f9f6ce9 },
	{ 0x4fd8ae7f }, { 0x5013401c }, { 0x50482254 }, { 0x50880156 }, { 0x50b8d9aa }, { 0x50fb3ccf },
	{ 0x512abbce }, { 0x51680d11 }, { 0x519db1ed }, { 0x51d6546b }, { 0x5211a6f5 }, { 0x5245f63b },
	{ 0x5286876d }, { 0x52b6d809 }, { 0x52f882b7 }, { 0x5328e166 },
};
static const fbits_t log2e_m1[2]   = { { 0x3ed20000 }, { 0x3d054765 } }; /* __log2e_m1$localstatic0$__log2f__Ff */
static const fbits_t log2_poly[2]  = { { 0xbf38aa80 }, { 0x3ef637a6 } }; /* @93 */
static const fbits_t log2_bias     = { 0x3fb00000 }; /* @247, 1.375 */
static const fbits_t exp2_c075     = { 0x3f400000 }; /* @248 */
static const fbits_t exp2_c025     = { 0x3e800000 }; /* @249 */
static const fbits_t expf_max      = { 0x42b17218 }; /* @259, 88.72284 */
static const fbits_t expf_min      = { 0xc2aeac50 }; /* @260, -87.33655 */
static const fbits_t expf_c1       = { 0x3f7e0000 }; /* @261, 1 - 1/128 */
static const fbits_t expf_c2       = { 0x3c000001 }; /* @262, about 1/128 */
static const fbits_t msl_nan       = { 0x7fffffff }; /* _nan */

#define MSL_HOT_PART "msl_math_pow.inc"
#include "msl_math_hot.h"
#undef MSL_HOT_PART
#if MSL_FMA_DISPATCH
float sms_msl_expf(float x) { return msl_have_fma() ? sms_msl_expf_hw(x) : sms_msl_expf_soft(x); }
float sms_msl_powf(float x, float y) { return msl_have_fma() ? sms_msl_powf_hw(x, y) : sms_msl_powf_soft(x, y); }
#endif

/* ---- MSL's inline std::fmodf (the weak 0x80109AFC in wireTrap.cpp) ----- */

/* if (fabsf(y) > fabsf(x)) return x; return x - y * (float)(s64)(x / y);
 * The DOL converts the quotient with the runtime's __cvt_dbl_usll (saturating
 * from 2^63, port_cvt_dbl_usll) and back with __cvt_sll_flt, which rounds the
 * 64-bit integer to double and then (frsp) to single, and the subtraction is
 * one fnmsubs: -(y * n - x), -0 when y * n == x. __cvt_dbl_usll saturates a
 * NaN by its sign, so fmodf(0, 0) is -0 on the console (its default NaN is
 * positive: 2^63 * 0) and fmodf(inf, inf) a NaN. JGeometry::TUtil<f32>::mod
 * (koopajr.cpp) is the same machine code. */
float sms_msl_fmodf(float x, float y)
{
	float q, n;
	if (fabsf(y) > fabsf(x))
		return x;
	q = x / y;
	if (q != q && x == x && y == y)
		q = u2f(0x7fc00000); /* 0 / 0, inf / inf: the Gekko's default NaN is positive (x86's is negative) */
	n = (float)(double)(int64_t)port_cvt_dbl_usll(q);
	return fnmsubs(y, n, x);
}

/* ---- MSL's inline std::sqrtf (math.h; the weak 0x80012C1C in MAnmSound.cpp) */

/* The console computes, at each of its call sites, in double precision:
 *   if (x > 0) { g = frsqrte(x); 3 times: g = .5 * g * (3 - g * g * x);
 *                return (float)(x * g); }  (3 - ... is one fnmsub)
 *   return x;
 * For every positive finite float that is the correctly rounded square root,
 * the host's sqrtf (tools/mslmath/check.sh runs the sequence with the Gekko's
 * estimate over all 2^32 bit patterns), so the host instruction stands in for
 * it there. It differs from sqrtf elsewhere: zero, negative numbers and NaN
 * come back unchanged, and +inf gives a NaN (frsqrte(+inf) is +0, and
 * 0 * inf is invalid). */
float sms_msl_sqrtf(float x)
{
	if (x > 0.0f) {
		if (x < float_huge.f)
			return sqrtf(x);
		return u2f(0x7fc00000); /* the Gekko's default NaN, rounded to single */
	}
	return x;
}

#ifdef MSL_MATH_TEST_HOOKS
/* The console's instruction sequence, for the check above. */
float msl_sqrtf_newton_for_test(float x)
{
	if (x > 0.0f) {
		double g = frsqrte((double)x), t;
		int i;
		for (i = 0; i < 3; i++) {
			t = g * g;
			g = 0.5 * g;
			t = fma((double)x, t, -3.0);
			PORT_OPAQUE(t);
			t = -t; /* fnmsub */
			g = g * t;
		}
		return (float)((double)x * g);
	}
	return x;
}
#endif

/* ---- The game's own frsqrte helpers (decomp-patches/fpu-06) ---------------
 *
 * Not MSL, but game and JSystem inlines that MWCC compiled the same way:
 * the Gekko's estimate, and a Newton-Raphson step whose 3 - ... is one fused
 * fnmsub. The DOL expands them at their call sites and keeps weak copies
 * (JGeometry::TUtil<f32>::sqrt and inv_sqrt in boid.o, MsSqrtf in cameragc.o),
 * which tools/mslmath/check.sh runs under qemu-ppc against these.
 *
 * TUtil<f32>::inv_sqrt, and sqrt with the final * mag, in single precision:
 *   root = frsp(frsqrte(mag)); rr = root * root; h = 0.5 * root;
 *   t = fnmsubs(mag, rr, 3) = -(mag * rr - 3) rounded once; h * t [* mag]
 * for mag > 0 or a NaN, and mag itself otherwise. root is a single, so the
 * Gekko's 25-bit rounding of an fmuls operand changes nothing. */
float sms_jg_inv_sqrtf(float mag)
{
	if (mag <= 0.0f)
		return mag;
	float root = (float)frsqrte(mag);
	float rr   = root * root;
	float h    = 0.5f * root;
	return h * fnmsubs(mag, rr, 3.0f);
}

float sms_jg_sqrtf(float mag)
{
	if (mag <= 0.0f)
		return mag;
	float root = (float)frsqrte(mag);
	float rr   = root * root;
	float h    = 0.5f * root;
	return h * fnmsubs(mag, rr, 3.0f) * mag;
}

/* MsSqrtf (MarioUtil/MathUtil.hpp): one step in double precision, then
 *   (float)(x * g)   with 3 - g * g * x one fnmsub, for x > 0; x otherwise.
 * Unlike std::sqrtf's three steps this is not the rounded square root (it
 * differs from sqrtf for 8.5% of the positive normal floats), so the sequence
 * itself is followed. Rounding the fnmsub once or twice gives the same result
 * for every positive normal float, but the DOL's fused form costs nothing.
 * TUtil's single-precision step is another matter: unfused, it differs for
 * 3.4% (sqrt) and 3.5% (inv_sqrt) of them. */
float sms_ms_sqrtf(float x)
{
	if (x > 0.0f) {
		double g = frsqrte((double)x), gg, h, t;
		gg = g * g;
		h  = 0.5 * g;
		t  = fma((double)x, gg, -3.0);
		PORT_OPAQUE(t);
		t = -t; /* fnmsub */
		return (float)((double)x * (h * t));
	}
	return x;
}

#ifdef MSL_MATH_TEST_HOOKS
/* JPASqrtf (JParticle/JPAMath.cpp) needs no port code: its C body,
 * x * (f32)__frsqrte(x) for x > 0 and 0 otherwise, is already the DOL's frsp
 * of the estimate and one fmuls. The same body, for the check. */
float msl_jpa_sqrtf_for_test(float x)
{
	if (x > 0.0f)
		return x * (float)frsqrte(x);
	return 0.0f;
}
#endif
