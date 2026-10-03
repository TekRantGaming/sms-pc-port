/* Host side of tools/mslmath/check.sh.
 *   harness gen N SEED FNS  writes N random test records for each function in
 *                       FNS (a comma-separated list of numbers) to stdout
 *   harness run         reads records from stdin and writes each result
 *   harness sqrt        compares MSL's sqrtf sequence with sms_msl_sqrtf on
 *                       all 2^32 bit patterns (-DMSL_MATH_TEST_HOOKS only)
 * A record is big-endian: u32 function, u32 0, f64 a, f64 b; a result is a
 * big-endian f64 (a float result widened, as the Gekko holds it in a register).
 * Functions: 0 sinf 1 cosf 2 tanf 3 atanf 4 atan2f(a, b) 5 acosf 6 atan
 * 7 atan2(a, b) 8 _inv_sqrtf 9 expf 10 powf(a, b) 11 std::fmodf(a, b)
 * 12 std::sqrtf 13 std::sqrtf's instruction sequence
 * 14 JGeometry::TUtil<f32>::mod(a, b) (sms_msl_fmodf here)
 * 15 JGeometry::TUtil<f32>::sqrt 16 TUtil<f32>::inv_sqrt 17 MsSqrtf 18 JPASqrtf;
 * 8, 13 and 18 need -DMSL_MATH_TEST_HOOKS. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "msl_math.h"

static uint64_t s;
static uint64_t rnd(void)
{
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double dbits(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static double uni(double lo, double hi) { return lo + (hi - lo) * (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

/* A float argument: arbitrary bit patterns, or values in the range the game
 * uses (angles, stick and vector components, cosines), or near special points. */
static float farg(int fn)
{
	switch (rnd() % 6) {
	case 0: return fbits((uint32_t)rnd());
	case 1: return (float)uni(-8, 8);
	case 2: return (float)uni(-1.05, 1.05);
	case 3: return (float)uni(-1000, 1000);
	case 4: return (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 60) - 40));
	default: {
		/* close to multiples of pi/4 and to the atanf breakpoints */
		static const float k[] = { 0.0f, 0.41421357f, 0.5345111f, 0.8206788f, 1.0f,
			1.2185035f, 1.8708684f, 2.4142137f, 0.78539819f, 1.5707964f, 3.1415927f, 4.712389f };
		float v = k[rnd() % 12];
		uint32_t u;
		memcpy(&u, &v, 4);
		u += (uint32_t)(rnd() % 64) - 32;
		v = fbits(u);
		return (rnd() & 1) ? -v : v;
	}
	}
}
/* expf: the whole range and past both ends, the audio code's small values */
static float earg(void)
{
	switch (rnd() % 5) {
	case 0: return fbits((uint32_t)rnd());
	case 1: return (float)uni(-100, 100);
	case 2: return (float)uni(-4, 4);
	case 3: {
		float v = (rnd() & 1) ? 88.72284f : -87.33655f;
		uint32_t u;
		memcpy(&u, &v, 4);
		return fbits(u + (uint32_t)(rnd() % 64) - 32);
	}
	default: return (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 40) - 30));
	}
}
/* powf: bases of any sign and size with integer, fractional and odd exponents
 * (the game squares distances with powf(d, 2.0f), raises random values in
 * [0, 1) to a slope, and takes powf(2e-4f, 1 / p)) */
static void parg(double* a, double* b)
{
	float x, y;
	switch (rnd() % 8) {
	case 0: x = fbits((uint32_t)rnd()); break;
	case 1: x = (float)uni(-20000, 20000); break;
	case 2: x = (float)uni(0, 1); break;
	case 3: x = (float)uni(-4, 4); break;
	case 4: x = (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 200) - 100)); break;
	case 5: x = (rnd() & 1) ? 0.0f : -0.0f; break;
	case 6: x = (float)(int)(rnd() % 41) - 20.0f; break;
	default: x = 0.00020000001f; break;
	}
	switch (rnd() % 8) {
	case 0: y = fbits((uint32_t)rnd()); break;
	case 1: y = 2.0f; break;
	case 2: y = (float)(int)(rnd() % 41) - 20.0f; break;
	case 3: y = (float)uni(-8, 8); break;
	case 4: y = (float)uni(0, 5); break;
	case 5: y = 1.0f / (float)uni(0.05, 10); break;
	case 6: y = (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 80) - 40)); break;
	default: y = (rnd() & 1) ? 0.0f : fbits(0x7f800000u | (uint32_t)(rnd() & 1) << 31); break;
	}
	*a = x;
	*b = y;
}
/* fmodf: the game's angle wraps (360 and 2 pi ranges), large quotients (past
 * 2^24, 2^53 and 2^63), exact multiples, zeros and specials */
static void marg(double* a, double* b)
{
	float x, y;
	switch (rnd() % 6) {
	case 0: x = fbits((uint32_t)rnd()); break;
	case 1: x = (float)uni(-1080, 1080); break;
	case 2: x = (float)uni(-20, 20); break;
	case 3: x = (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 140) - 20)); break;
	case 4: x = (float)((int)(rnd() % 13) - 6) * 360.0f; break;
	default: x = (float)uni(0, 720); break;
	}
	switch (rnd() % 6) {
	case 0: y = fbits((uint32_t)rnd()); break;
	case 1: y = 360.0f; break;
	case 2: y = 6.2831855f; break;
	case 3: y = (float)uni(-50, 50); break;
	case 4: y = (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 80) - 60)); break;
	default: y = (rnd() & 3) ? (float)uni(0.5, 2) : 0.0f; break;
	}
	*a = x;
	*b = y;
}
/* sqrtf: squared distances and lengths, tiny, negative and special values */
static float sarg(void)
{
	switch (rnd() % 5) {
	case 0: return fbits((uint32_t)rnd());
	case 1: return (float)uni(0, 1e8);
	case 2: return (float)uni(-2, 2);
	case 3: return (float)(uni(0, 1) * ldexp(1.0, (int)(rnd() % 280) - 150));
	default: {
		float v = (float)(int)(rnd() % 4096);
		v *= v;
		return fbits(fb(v) + (uint32_t)(rnd() % 5) - 2);
	}
	}
}
static double darg(void)
{
	switch (rnd() % 4) {
	case 0: return dbits(rnd());
	case 1: return uni(-8, 8);
	case 2: return uni(-1, 1) * ldexp(1.0, (int)(rnd() % 80) - 40);
	default: return (double)farg(0);
	}
}

static void put64(uint64_t v, FILE* f)
{
	for (int i = 7; i >= 0; i--)
		fputc((int)(v >> (i * 8)) & 0xff, f);
}
static uint64_t get64(const unsigned char* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}
static uint64_t dbl2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

#ifdef MSL_MATH_TEST_HOOKS
float msl_inv_sqrtf_for_test(float);
float msl_sqrtf_newton_for_test(float);
float msl_jpa_sqrtf_for_test(float);
#endif
#ifdef HARNESS_QEMU
double qemu_frsqrte(double x) { return 1.0 / sqrt(x); } /* qemu's "estimate" */
#endif

int main(int argc, char** argv)
{
	if (argc == 5 && !strcmp(argv[1], "gen")) {
		long n        = atol(argv[2]);
		const char* p = argv[4];
		s             = strtoull(argv[3], 0, 0) | 1;
		while (*p) {
			char* end;
			uint32_t fn = (uint32_t)strtoul(p, &end, 10);
			p           = *end ? end + 1 : end;
			for (long i = 0; i < n; i++) {
				double a, b = 0;
				if (fn == 6 || fn == 7) {
					a = darg();
					if (fn == 7)
						b = darg();
				} else if (fn == 9) {
					a = earg();
				} else if (fn == 10) {
					parg(&a, &b);
				} else if (fn == 11 || fn == 14) {
					marg(&a, &b);
				} else if (fn == 12 || fn == 13 || (fn >= 15 && fn <= 18)) {
					a = sarg();
				} else {
					a = farg(fn);
					if (fn == 4)
						b = farg(fn);
				}
				put64((uint64_t)fn << 32, stdout);
				put64(dbl2u(a), stdout);
				put64(dbl2u(b), stdout);
			}
		}
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "run")) {
		unsigned char r[24];
		while (fread(r, 24, 1, stdin) == 1) {
			uint32_t fn = (uint32_t)(get64(r) >> 32);
			double a = dbits(get64(r + 8)), b = dbits(get64(r + 16)), y = 0;
			switch (fn) {
			case 0: y = sms_msl_sinf((float)a); break;
			case 1: y = sms_msl_cosf((float)a); break;
			case 2: y = sms_msl_tanf((float)a); break;
			case 3: y = sms_msl_atanf((float)a); break;
			case 4: y = sms_msl_atan2f((float)a, (float)b); break;
			case 5: y = sms_msl_acosf((float)a); break;
			case 6: y = sms_msl_atan(a); break;
			case 7: y = sms_msl_atan2(a, b); break;
			case 9: y = sms_msl_expf((float)a); break;
			case 10: y = sms_msl_powf((float)a, (float)b); break;
			case 11: y = sms_msl_fmodf((float)a, (float)b); break;
			case 12: y = sms_msl_sqrtf((float)a); break;
			case 14: y = sms_msl_fmodf((float)a, (float)b); break;
			case 15: y = sms_jg_sqrtf((float)a); break;
			case 16: y = sms_jg_inv_sqrtf((float)a); break;
			case 17: y = sms_ms_sqrtf((float)a); break;
#ifdef MSL_MATH_TEST_HOOKS
			case 8: y = msl_inv_sqrtf_for_test((float)a); break;
			case 13: y = msl_sqrtf_newton_for_test((float)a); break;
			case 18: y = msl_jpa_sqrtf_for_test((float)a); break;
#endif
			}
			put64(dbl2u(y), stdout);
		}
		return 0;
	}
#ifdef MSL_MATH_TEST_HOOKS
	if (argc == 2 && !strcmp(argv[1], "sqrt")) {
		uint64_t bad = 0;
		for (uint64_t u = 0; u <= 0xffffffffu; u++) {
			float x = fbits((uint32_t)u), p = msl_sqrtf_newton_for_test(x), q = sms_msl_sqrtf(x);
			if (fb(p) != fb(q) && !(p != p && q != q) && bad++ < 3)
				printf("    %08x: %08x vs %08x\n", (uint32_t)u, fb(p), fb(q));
		}
		printf("std::sqrtf  4294967296 inputs, %llu differ\n", (unsigned long long)bad);
		return bad != 0;
	}
#endif
	fprintf(stderr, "usage: harness gen N SEED FNS | harness run | harness sqrt\n");
	return 2;
}
