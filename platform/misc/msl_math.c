/* The GameCube's MSL trigonometry, for the host.
 *
 * The game calls the maths library that shipped in its DOL, not the host's:
 *   MSL_C.PPCEABI.bare.H.a  trigf.c         sinf, cosf, tanf
 *                           inverse_trig.c  atanf, atan2f, acosf, _inv_sqrtf
 *                           s_atan.c        atan    (fdlibm)
 *                           e_atan2.c       __ieee754_atan2 (fdlibm)
 *                           w_atan2.c       atan2   (the fdlibm wrapper)
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
 *   fmadds a,c,b             (float)((double)a * c + b): the Gekko rounds the
 *                            fused result to double and then to single (the
 *                            model Dolphin implements). The product of two
 *                            singles is exact in double, so one double
 *                            addition is that fused double result.
 *   fnmsubs / fnmadds        the same, negated (exact under round-to-nearest).
 *   fmadd/fmsub/fnmsub       fma(), fused in double (glibc's is exact).
 *   fctiwz                   truncation that saturates like the Gekko's.
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

/* Gekko single-precision fused multiply-adds. MSL_MATH_SINGLE_ROUNDING (a
 * test-only switch) rounds once, to single, which is what qemu-ppc does. */
#ifdef MSL_MATH_SINGLE_ROUNDING
static inline float fmadds(float a, float c, float b) { return fmaf(a, c, b); }
static inline float fnmadds(float a, float c, float b) { return -fmaf(a, c, b); }
static inline float fnmsubs(float a, float c, float b) { return fmaf(-a, c, b); }
#else
static inline float fmadds(float a, float c, float b) { return (float)((double)a * c + b); }
static inline float fnmadds(float a, float c, float b) { return (float)-((double)a * c + b); }
static inline float fnmsubs(float a, float c, float b) { return (float)(b - (double)a * c); }
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

/* The argument reduction shared by sinf and cosf: x less the nearest multiple
 * of pi/2, in units of pi/4 (frac_part), and that multiple's quadrant. */
static float trig_reduce(float x, int* quadrant)
{
	float z = two_over_pi.f * x;                                    /* fmuls */
	int32_t n = (f2u(x) & 0x80000000) ? fctiwz(z - half.f) : fctiwz(half.f + z);
	float n2  = (float)(int32_t)((uint32_t)n << 1); /* slwi, the 0x43300000 conversion, fsubs */
	float f   = x - n2;                                             /* fsubs */
	f         = fmadds(four_over_pi_m1[0].f, x, f);
	f         = fmadds(four_over_pi_m1[1].f, x, f);
	f         = fmadds(four_over_pi_m1[2].f, x, f);
	f         = fmadds(four_over_pi_m1[3].f, x, f);
	*quadrant = (int)(n & 3);
	return f;
}

float sms_msl_cosf(float x)
{
	int n;
	float f = trig_reduce(x, &n), xsq, t;
	n <<= 1;
	if (fabsf(f) < sqrt_eps.f)
		return fnmsubs(f, Q(n), Q(n + 1));
	xsq = f * f;
	if (n & 2) {
		t = fmadds(P(1), xsq, P(3));
		t = fmadds(xsq, t, P(5));
		t = fmadds(xsq, t, P(7));
		t = fnmadds(xsq, t, P(9));
		t = f * t;
		return t * Q(n);
	}
	t = fmadds(P(0), xsq, P(2));
	t = fmadds(xsq, t, P(4));
	t = fmadds(xsq, t, P(6));
	t = fmadds(xsq, t, P(8));
	return t * Q(n + 1);
}

float sms_msl_sinf(float x)
{
	int n;
	float f = trig_reduce(x, &n), xsq, t;
	n <<= 1;
	if (fabsf(f) < sqrt_eps.f)
		return fmadds(P(9), f * Q(n + 1), Q(n));
	xsq = f * f;
	if (n & 2) {
		t = fmadds(P(0), xsq, P(2));
		t = fmadds(xsq, t, P(4));
		t = fmadds(xsq, t, P(6));
		t = fmadds(xsq, t, P(8));
		return t * Q(n);
	}
	t = fmadds(P(1), xsq, P(3));
	t = fmadds(xsq, t, P(5));
	t = fmadds(xsq, t, P(7));
	t = fmadds(xsq, t, P(9));
	t = f * t;
	return t * Q(n + 1);
}

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
