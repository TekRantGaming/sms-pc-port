/* tools/quantize/check.sh: port_gekko_quantize (src/port_fpu.h), the Gekko's
 * quantised integer store, against a separate statement of the same model
 * (Dolphin's interpreter: the value as a single, times 2^scale in single
 * precision, clamped to the type's range, converted towards zero; NaN 0).
 *   test all     every float bit pattern, store type s8 scale 0 (OSf32tos8's
 *                GQR4), and each of u8, u16, s8, s16 at every scale on a
 *                sample; prints a checksum of the s8 results
 *   test table   the s8 results of a few values, for the record */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port_fpu.h"

static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static long long ref(float x, unsigned type, int scale)
{
	static const double lo[] = { 0, 0, -128, -32768 }, hi[] = { 255, 65535, 127, 32767 };
	float s = (float)ldexp(1.0, scale);
	volatile float v = x * s; /* one single-precision rounding */
	double d = v;
	if (isnan(d))
		return 0;
	d = fmin(fmax(d, lo[type - 4]), hi[type - 4]);
	return (long long)trunc(d);
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "table")) {
		static const float v[] = { 0.0f, -0.0f, 0.5f, 0.99999994f, 1.0f, -1.5f, 89.6f, -89.6f,
			126.99999f, 127.0f, 127.5f, 128.0f, -128.0f, -128.5f, -129.0f, 1e9f, -1e9f, INFINITY, -INFINITY, NAN };
		for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
			printf("%-12.9g -> %4d\n", v[i], (signed char)port_gekko_quantize(v[i], 6, 0));
		return 0;
	}
	uint64_t bad = 0, sum = 0;
	uint32_t u = 0;
	do {
		float x = fbits(u);
		int q = port_gekko_quantize(x, 6, 0);
		if (q != ref(x, 6, 0)) {
			if (bad++ < 5)
				printf("s8 %08x: %d, model %lld\n", u, q, ref(x, 6, 0));
		}
		sum = sum * 31 + (uint8_t)q;
	} while (++u);
	uint64_t s = 0x5eed;
	for (unsigned type = 4; type <= 7; type++)
		for (unsigned scale = 0; scale < 64; scale++)
			for (int i = 0; i < 200000; i++) {
				s ^= s << 13; s ^= s >> 7; s ^= s << 17;
				float x = (i & 1) ? fbits((uint32_t)s) : (float)((int)(s % 200001) - 100000) / 64.0f;
				int e = (int)scale - (scale & 32 ? 64 : 0);
				if (port_gekko_quantize(x, type, scale) != ref(x, type, e)) {
					if (bad++ < 10)
						printf("type %u scale %u %08x: %d, model %lld\n", type, scale, (unsigned)s,
						       port_gekko_quantize(x, type, scale), ref(x, type, e));
				}
			}
	printf("%llu differ; s8 checksum %016llx\n", (unsigned long long)bad, (unsigned long long)sum);
	return bad != 0;
}
