/* Host side of tools/mtxmath/check.sh.
 *   harness gen N SEED   writes N random records for each case (table.h) to stdout
 *   harness run          reads records from stdin and writes each result
 *   harness cmp IN A B   compares two result files, per case
 * Record and result formats: gen.py. Link with an MTX object (platform/mtx)
 * and msl_math. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "table.h"
#define REC (8 + 4 * NIN)
#define RES (4 * NOUT + 4)

static uint64_t s;
static uint64_t rnd(void)
{
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static double uni(double lo, double hi) { return lo + (hi - lo) * (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

/* One input value in the given style. */
static float val(int style)
{
	static const float special[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, INFINITY, -INFINITY, NAN, 1e-40f, 3.4e38f };
	switch (style) {
	case 0: return fbits((uint32_t)rnd()); /* any bit pattern */
	case 1: return (float)uni(-1, 1);       /* unit vectors, rotations */
	case 2: return (float)uni(-3000, 3000); /* positions */
	case 3: return (float)(uni(-1, 1) * ldexp(1.0, (int)(rnd() % 48) - 24));
	case 4: /* few significant bits: exact products, cancellations and ties */
		return (float)ldexp((double)((int)(rnd() % 4097) - 2048), (int)(rnd() % 24) - 16);
	default: /* unit-range values with specials mixed in */
		if (rnd() % 6 == 0)
			return special[rnd() % (sizeof special / sizeof *special)];
		return (float)uni(-1, 1);
	}
}

#ifdef HARNESS_QEMU
double qemu_frsqrte(double x) { return 1.0 / sqrt(x); } /* qemu's "estimate", for msl_math.c */
#endif

static void put32(uint32_t v, FILE* f)
{
	fputc(v >> 24, f), fputc(v >> 16 & 255, f), fputc(v >> 8 & 255, f), fputc(v & 255, f);
}
static uint32_t get32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char** argv)
{
	if (argc == 4 && !strcmp(argv[1], "gen")) {
		long n = atol(argv[2]);
		s      = strtoull(argv[3], 0, 0) | 1;
		for (unsigned c = 0; c < NCASES; c++) {
			for (long i = 0; i < n; i++) {
				int style = (int)(rnd() % 6);
				unsigned aux;
				float in[NIN];
				if (!strncmp(case_names[c], "MultVecArray", 12))
					aux = 2 + (unsigned)(rnd() % 4); /* the DOL loops 2^32 times for 1 */
				else if (!strncmp(case_names[c], "J3DConcatIndexed", 16)) /* two u16 matrix indices */
					aux = (unsigned)(rnd() % 2) << 16 | (unsigned)(rnd() % 2);
				else if (!strcmp(case_names[c], "J3DWeightEnvelope")) /* mix count, two indices */
					aux = (1 + (unsigned)(rnd() % 2)) << 16 | (unsigned)(rnd() % 2) << 8 | (unsigned)(rnd() % 2);
				else
					aux = (unsigned char)"xyzXYZa\0w"[rnd() % 9];
				put32(c, stdout);
				put32(aux, stdout);
				for (int k = 0; k < NIN; k++) {
					in[k] = val(style);
					/* the perspective cases: angles and planes in their ranges, mostly */
					if (style == 1 && strstr(case_names[c], "Perspective"))
						in[k] = (float)uni(1, 179);
				}
				/* J3DHermiteS: s16 keys in the high halves of in[1..6], and
				 * mostly a frame between the two times */
				if (!strcmp(case_names[c], "J3DHermiteS") && style != 0) {
					for (int k = 1; k <= 6; k++)
						in[k] = fbits((uint32_t)rnd());
					uint32_t t0, t1;
					memcpy(&t0, &in[1], 4), memcpy(&t1, &in[4], 4);
					in[0] = (float)((short)(t0 >> 16) + uni(-0.25, 1.25) * ((short)(t1 >> 16) - (short)(t0 >> 16)));
				}
				for (int k = 0; k < NIN; k++) {
					uint32_t u;
					memcpy(&u, &in[k], 4);
					put32(u, stdout);
				}
			}
		}
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "run")) {
		unsigned char r[REC];
		while (fread(r, REC, 1, stdin) == 1) {
			float in[NIN], out[NOUT];
			uint32_t o[NOUT + 1];
			unsigned ret = 0;
			for (int k = 0; k < NIN; k++) {
				uint32_t u = get32(r + 8 + 4 * k);
				memcpy(&in[k], &u, 4);
			}
			for (int k = 0; k < NOUT; k++)
				out[k] = fbits(0x13579bdf);
			run_case(get32(r), get32(r + 4), in, out, &ret);
			memcpy(o, out, 4 * NOUT);
			o[NOUT] = ret;
			for (int k = 0; k < NOUT + 1; k++)
				put32(o[k], stdout);
		}
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "cmp")) {
		/* per case, the records whose results differ in any bit (two NaNs are equal) */
		FILE *fi = fopen(argv[2], "rb"), *fa = fopen(argv[3], "rb"), *fb = fopen(argv[4], "rb");
		unsigned char r[REC], a[RES], b[RES];
		long tot[NCASES] = { 0 }, bad[NCASES] = { 0 }, total = 0;
		int shown[NCASES] = { 0 };
		if (!fi || !fa || !fb)
			return 2;
		while (fread(r, REC, 1, fi) == 1) {
			unsigned c = get32(r) % NCASES;
			int differ = 0;
			if (fread(a, RES, 1, fa) != 1 || fread(b, RES, 1, fb) != 1) {
				fprintf(stderr, "short results\n");
				return 2;
			}
			tot[c]++;
			for (int k = 0; k < NOUT + 1; k++) {
				uint32_t x = get32(a + 4 * k), y = get32(b + 4 * k);
				int nx = (x & 0x7f800000) == 0x7f800000 && (x & 0x7fffff), ny = (y & 0x7f800000) == 0x7f800000 && (y & 0x7fffff);
				if (x != y && !(nx && ny))
					differ = 1;
			}
			if (!differ)
				continue;
			bad[c]++, total++;
			if (shown[c]++ < 2) {
				printf("  %s aux %u in", case_names[c], get32(r + 4));
				for (int k = 0; k < 16; k++)
					printf(" %08x", get32(r + 8 + 4 * k));
				printf("\n    out");
				for (int k = 0; k < NOUT + 1; k++)
					printf(" %08x/%08x", get32(a + 4 * k), get32(b + 4 * k));
				printf("\n");
			}
		}
		for (unsigned c = 0; c < NCASES; c++)
			printf("%-22s %9ld inputs, %ld differ\n", case_names[c], tot[c], bad[c]);
		return total ? 1 : 0;
	}
	fprintf(stderr, "usage: harness gen N SEED | harness run | harness cmp RECORDS A B\n");
	return 2;
}
