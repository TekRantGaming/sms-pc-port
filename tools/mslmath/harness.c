/* Host side of tools/mslmath/check.sh.
 *   harness gen N SEED [LAST]  writes N random test records for each function
 *                       0 to LAST (default 7) to stdout
 *   harness run         reads records from stdin and writes each result
 * A record is big-endian: u32 function, u32 0, f64 a, f64 b; a result is a
 * big-endian f64 (a float result widened, as the Gekko holds it in a register).
 * Functions: 0 sinf 1 cosf 2 tanf 3 atanf 4 atan2f(a, b) 5 acosf 6 atan
 * 7 atan2(a, b) 8 _inv_sqrtf (with -DMSL_MATH_TEST_HOOKS only). */
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

#ifdef HARNESS_QEMU
double qemu_frsqrte(double x) { return 1.0 / sqrt(x); } /* qemu's "estimate" */
#endif

int main(int argc, char** argv)
{
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "gen")) {
		long n        = atol(argv[2]);
		uint32_t last = argc == 5 ? (uint32_t)atoi(argv[4]) : 7;
		s             = strtoull(argv[3], 0, 0) | 1;
		for (uint32_t fn = 0; fn <= last; fn++) {
			for (long i = 0; i < n; i++) {
				double a, b = 0;
				if (fn == 6 || fn == 7) {
					a = darg();
					if (fn == 7)
						b = darg();
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
#ifdef MSL_MATH_TEST_HOOKS
			case 8: {
				extern float msl_inv_sqrtf_for_test(float);
				y = msl_inv_sqrtf_for_test((float)a);
				break;
			}
#endif
			}
			put64(dbl2u(y), stdout);
		}
		return 0;
	}
	fprintf(stderr, "usage: harness gen N SEED [LAST] | harness run\n");
	return 2;
}
