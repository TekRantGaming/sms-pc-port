// JSystem's and the game's own paired-single routines, instruction for
// instruction (included by mtx.cpp after mtx_ps.inc, once per fused
// multiply-add implementation, with the same helpers and rounding model).
//
// The decomp spells these as Gekko assembly for MWCC (J3DTransform.cpp,
// J3DTransform.hpp, J3DModel.cpp, J3DAnimation.cpp, MathUtil.cpp); the port
// patch fpu-01-paired-single-routines makes their host builds call the
// functions below. Each follows the DOL's machine code (the decomp's split of
// the original objects, build/GMSE01/obj), register for register.

static const f32 jUnit01[2]      = { 0.0f, 1.0f };  // Unit01 (J3DTransform.cpp), J3DUnit01 (J3DModel.cpp)
static const f32 jPSMulUnit01[2] = { 0.0f, -1.0f }; // PSMulUnit01 (J3DTransform.cpp)

// --- J3DTransform.cpp ---

// The transpose of the inverse of src's 3x3 part (PSMTXInverse's code with a
// second Newton step on the reciprocal and no transposing merges).
static u32 gJ3DPSCalcInverseTranspose(const f32 (*src)[4], f32 (*dst)[3])
{
	ps f0  = L1(&src[0][0]);
	ps f1  = L(&src[0][1]);
	ps f2  = L1(&src[1][0]);
	ps f6  = m10(f1, f0);
	ps f3  = L(&src[1][1]);
	ps f4  = L1(&src[2][0]);
	ps f7  = m10(f3, f2);
	ps f5  = L(&src[2][1]);
	ps f11 = mul(f3, f6);
	ps f8  = m10(f5, f4);
	ps f13 = mul(f5, f7);
	f11    = msub(f1, f7, f11);
	ps f12 = mul(f1, f8);
	f13    = msub(f3, f8, f13);
	f12    = msub(f5, f6, f12);
	ps f10 = mul(f3, f4);
	ps f9  = mul(f0, f5);
	f8     = mul(f1, f2);
	f10    = msub(f2, f5, f10);
	f9     = msub(f1, f4, f9);
	f8     = msub(f0, f3, f8);
	f7     = mul(f0, f13);
	f1     = sub(f1, f1);
	f7     = madd(f2, f12, f7);
	f7     = madd(f4, f11, f7);
	if (f7.a == f1.a) // ps_cmpo0; bne: NaN goes on
		return 0;
	f0  = F(FRES(f7.a));
	f6  = add(f0, f0);
	f5  = mul(f0, f0);
	f0  = nmsub(f7, f5, f6);
	f6  = add(f0, f0);
	f5  = mul(f0, f0);
	f0  = nmsub(f7, f5, f6);
	f13 = muls0(f13, f0);
	f12 = muls0(f12, f0);
	S(&dst[0][0], f13);
	f11 = muls0(f11, f0);
	S(&dst[1][0], f12);
	f10 = muls0(f10, f0);
	S(&dst[2][0], f11);
	f9 = muls0(f9, f0);
	S1(&dst[0][2], f10);
	f8 = muls0(f8, f0);
	S1(&dst[1][2], f9);
	S1(&dst[2][2], f8);
	return 1;
}

// ab (3x4) = a (3x4) * b (4x4).
static void gJ3DMtxProjConcat(const f32 (*a)[4], const f32 (*b)[4], f32 (*ab)[4])
{
	for (int r = 0; r < 3; r++) {
		ps f2 = L(&a[r][0]), f3 = L(&a[r][2]);
		ps f6 = m00(f2, f2), f7 = m11(f2, f2), f8 = m00(f3, f3), f9 = m11(f3, f3);
		for (int c = 0; c < 4; c += 2) {
			ps f10 = L(&b[0][c]), f11 = L(&b[1][c]), f12 = L(&b[2][c]), f13 = L(&b[3][c]);
			ps f0  = mul(f6, f10);
			f0     = madd(f7, f11, f0);
			f0     = madd(f8, f12, f0);
			f0     = madd(f9, f13, f0);
			S(&ab[r][c], f0);
		}
	}
}

// dst[i] = a * b[idx[i]] for i < n. The console handles n = 0 as 2^32; here
// it is 1 (the first matrix, which the console also writes first).
static void gJ3DMTXConcatArrayIndexedSrc(const f32 (*a)[4], const f32 (*b)[3][4], const u16* idx, f32 (*dst)[3][4],
                                         u32 n)
{
	const f32* m = &a[0][0];
	f32* d       = &dst[0][0][0];
	ps f0 = L(m + 0), f2 = L(m + 4), f4 = L(m + 8);
	const f32* s = &b[*idx][0][0];
	ps f6 = L(s + 0);
	ps f9 = muls0(f6, f0);
	ps f7 = L(s + 4);
	ps f10 = muls0(f6, f2), f11 = muls0(f6, f4);
	ps f8 = L(s + 8);
	f9    = madds1(f7, f0, f9);
	ps f1 = L(m + 2);
	f10   = madds1(f7, f2, f10);
	ps f3 = L(m + 6);
	f11   = madds1(f7, f4, f11);
	ps f5 = L(m + 10);
	f9    = madds0(f8, f1, f9);
	f6    = L(s + 2);
	f10   = madds0(f8, f3, f10);
	f11   = madds0(f8, f5, f11);
	f7    = L(s + 6);
	S(d + 0, f9);
	f8     = muls0(f6, f0);
	f9     = muls0(f6, f2);
	ps f12 = muls0(f6, f4);
	f6     = L(s + 10);
	S(d + 4, f10);
	f8     = madds1(f7, f0, f8);
	f9     = madds1(f7, f2, f9);
	f12    = madds1(f7, f4, f12);
	ps f13 = L(jUnit01);
	S(d + 8, f11);
	for (;;) {
		f8  = madds0(f6, f1, f8);
		f9  = madds0(f6, f3, f9);
		f12 = madds0(f6, f5, f12);
		f8  = madd(f13, f1, f8);
		f9  = madd(f13, f3, f9);
		f12 = madd(f13, f5, f12);
		S(d + 2, f8);
		S(d + 6, f9);
		S(d + 10, f12);
		if (n <= 1) // bdz
			break;
		n--;
		d += 12;
		s   = &b[*++idx][0][0];
		f6  = L(s + 0);
		f9  = muls0(f6, f0);
		f7  = L(s + 4);
		f10 = muls0(f6, f2);
		f11 = muls0(f6, f4);
		f8  = L(s + 8);
		f9  = madds1(f7, f0, f9);
		f10 = madds1(f7, f2, f10);
		f11 = madds1(f7, f4, f11);
		f6  = L(s + 2);
		f9  = madds0(f8, f1, f9);
		f10 = madds0(f8, f3, f10);
		f11 = madds0(f8, f5, f11);
		f7  = L(s + 6);
		S(d + 0, f9);
		f8  = muls0(f6, f0);
		f9  = muls0(f6, f2);
		f12 = muls0(f6, f4);
		f6  = L(s + 10);
		S(d + 4, f10);
		f8  = madds1(f7, f0, f8);
		f9  = madds1(f7, f2, f9);
		f12 = madds1(f7, f4, f12);
		S(d + 8, f11);
	}
}

// dst[i] = a * b[i] for i < n (n = 0 as 1, as above).
static void gJ3DPSMtxArrayConcat(const f32 (*a)[4], const f32 (*b)[4], f32 (*dst)[4], u32 n)
{
	const f32* m = &a[0][0];
	const f32* s = &b[0][0];
	f32* d       = &dst[0][0];
	if (n == 0)
		n = 1;
	do {
		ps f0   = L(m + 0);
		ps f6   = L(s + 0);
		ps f7   = L(s + 2);
		ps f8   = L(s + 4);
		ps f12  = muls0(f6, f0);
		ps f2   = L(m + 4);
		ps f13  = muls0(f7, f0);
		ps f31  = L(jUnit01);
		ps f14  = muls0(f6, f2);
		ps f9   = L(s + 6);
		ps f15  = muls0(f7, f2);
		ps f1   = L(m + 2);
		f12     = madds1(f8, f0, f12);
		ps f3   = L(m + 6);
		f14     = madds1(f8, f2, f14);
		ps f10  = L(s + 8);
		f13     = madds1(f9, f0, f13);
		ps f11  = L(s + 10);
		s      += 12;
		f15     = madds1(f9, f2, f15);
		ps f4   = L(m + 8);
		ps f5   = L(m + 10);
		f12     = madds0(f10, f1, f12);
		f13     = madds0(f11, f1, f13);
		f14     = madds0(f10, f3, f14);
		f15     = madds0(f11, f3, f15);
		S(d + 0, f12);
		f2  = muls0(f6, f4);
		f13 = madds1(f31, f1, f13);
		f0  = muls0(f7, f4);
		S(d + 4, f14);
		f15 = madds1(f31, f3, f15);
		S(d + 2, f13);
		f2 = madds1(f8, f4, f2);
		f0 = madds1(f9, f4, f0);
		f2 = madds0(f10, f5, f2);
		S(d + 6, f15);
		f0 = madds0(f11, f5, f0);
		S(d + 8, f2);
		f0 = madds1(f31, f5, f0);
		S(d + 10, f0);
		d += 12;
	} while (--n != 0);
}

// --- J3DTransform.hpp (inlined by J3DSkinDeform::deform) ---

// d = m * (v, 1) for a 3x4 m.
static void gJ3DPSMulMtxVec(const f32 (*mtx)[4], const f32* v, f32* d)
{
	const f32* m = &mtx[0][0];
	ps f0  = L(v);
	ps f2  = L(m + 0);
	ps f1  = L1(v + 2);
	ps f4  = mul(f2, f0);
	ps f3  = L(m + 2);
	ps f5  = madd(f3, f1, f4);
	ps f8  = L(m + 4);
	ps f6  = sum0(f5, f5, f5); // frC's ps1 is never stored
	ps f9  = L(m + 6);
	ps f10 = mul(f8, f0);
	S1(d + 0, f6);
	ps f11 = madd(f9, f1, f10);
	f2     = L(m + 8);
	ps f12 = sum0(f11, f11, f11);
	f3     = L(m + 10);
	f4     = mul(f2, f0);
	S1(d + 1, f12);
	f5 = madd(f3, f1, f4);
	f6 = sum0(f5, f5, f5);
	S1(d + 2, f6);
}

// d = m * v for a 3x3 m: v's z goes through PSMulUnit01, (0 + z, -1 + 1).
static void gJ3DPSMulMtxVec33(const f32 (*mtx)[3], const f32* v, f32* d)
{
	const f32* m = &mtx[0][0];
	ps f0  = L(v);
	ps f2  = L(m + 0);
	ps f13 = L(jPSMulUnit01);
	ps f1  = L1(v + 2);
	f1     = add(f13, f1);
	ps f3  = L1(m + 2);
	ps f4  = mul(f2, f0);
	ps f8  = L(m + 3);
	ps f5  = madd(f3, f1, f4);
	ps f6  = sum0(f5, f5, f5);
	ps f9  = L1(m + 5);
	ps f10 = mul(f8, f0);
	S1(d + 0, f6);
	ps f11 = madd(f9, f1, f10);
	f2     = L(m + 6);
	ps f12 = sum0(f11, f11, f11);
	f3     = L1(m + 8);
	f4     = mul(f2, f0);
	S1(d + 1, f12);
	f5 = madd(f3, f1, f4);
	f6 = sum0(f5, f5, f5);
	S1(d + 2, f6);
}

// --- J3DModel.cpp: one mix matrix of J3DModel::calcWeightEnvelopeMtx ---

// acc += weight * (world * inv), in the DOL's accumulators: f9, f11, f13
// (columns 0-1 of rows 0-2) and f10, f12, f31 (columns 2-3).
static void gJ3DWeightEnvelopeMix(f32 (*acc)[4], const f32 (*world)[4], const f32 (*inv)[4], f32 weight)
{
	ps f27 = L(jUnit01);
	ps f9 = L(&acc[0][0]), f11 = L(&acc[1][0]), f13 = L(&acc[2][0]);
	ps f10 = L(&acc[0][2]), f12 = L(&acc[1][2]), f31 = L(&acc[2][2]);
	ps f0  = L(&inv[0][0]);
	ps f1  = L(&world[0][0]);
	ps f3  = L(&world[1][0]);
	ps f5  = L(&world[2][0]);
	ps f8  = muls0(f0, f1);
	ps f6  = L(&inv[1][0]);
	ps f30 = muls0(f0, f3);
	ps f29 = muls0(f0, f5);
	ps f7  = L(&inv[2][0]);
	f8     = madds1(f6, f1, f8);
	ps f2  = L(&world[0][2]);
	f30    = madds1(f6, f3, f30);
	ps f4  = L(&world[1][2]);
	f29    = madds1(f6, f5, f29);
	f6     = L(&world[2][2]);
	f8     = madds0(f7, f2, f8);
	f0     = F(weight);
	f30    = madds0(f7, f4, f30);
	f29    = madds0(f7, f6, f29);
	f7     = L(&inv[0][2]);
	f9     = madds0(f8, f0, f9);
	f11    = madds0(f30, f0, f11);
	f13    = madds0(f29, f0, f13);
	f8     = L(&inv[1][2]);
	f30    = muls0(f7, f1);
	f29    = muls0(f7, f3);
	ps f28 = muls0(f7, f5);
	f7     = L(&inv[2][2]);
	f30    = madds1(f8, f1, f30);
	f29    = madds1(f8, f3, f29);
	f28    = madds1(f8, f5, f28);
	f30    = madds0(f7, f2, f30);
	f29    = madds0(f7, f4, f29);
	f28    = madds0(f7, f6, f28);
	f30    = madd(f27, f2, f30);
	f29    = madd(f27, f4, f29);
	f28    = madd(f27, f6, f28);
	f10    = madds0(f30, f0, f10);
	f12    = madds0(f29, f0, f12);
	f31    = madds0(f28, f0, f31);
	S(&acc[0][0], f9), S(&acc[1][0], f11), S(&acc[2][0], f13);
	S(&acc[0][2], f10), S(&acc[1][2], f12), S(&acc[2][2], f31);
}

// --- J3DAnimation.cpp ---

// J3DHermiteInterpolationS, from its keys as floats (psq_l through GQR5, s16
// with no scale: exact).
static f32 gJ3DHermiteInterpolationS(f32 t, f32 time0, f32 value0, f32 tangent0, f32 time1, f32 value1, f32 tangent1)
{
	f32 f4 = time1 - time0;          // fsubs f4, f3, f0
	f32 f6 = t - time0;              // fsubs f6, f1, f0
	f32 f5 = value1 - value0;        // fsubs f5, f3, f2
	f6     = f6 / f4;                // fdivs
	f32 f1 = FMA(tangent1, f4, value0); // fmadds f1, f1, f4, f2
	f32 f7 = f6 * f6;                // fmuls
	f5     = NFMA(f4, tangent0, -f5); // fnmsubs f5, f4, f0, f5
	f1     = f1 - value1;            // fsubs
	f1     = f1 - f5;                // fsubs
	f32 f3 = f7 * f1;                // fmuls
	f1     = FMA(f4, tangent0, f3);  // fmadds f1, f4, f0, f3
	f1     = FMA(f1, f6, value0);    // fmadds f1, f1, f6, f2
	f1     = FMA(f5, f7, f1);        // fmadds f1, f5, f7, f1
	return f1 - f3;                  // fsubs
}

// --- MathUtil.cpp ---

// |v|² times its frsqrte estimate (no Newton step): |v| to about 1/4096, and
// 0 for 0.
static f32 gMsVECMag2(const f32* v)
{
	ps f3 = L(v);
	f3    = mul(f3, f3);
	ps f4 = F(v[2]);
	ps f5 = madd(f4, f4, f3);
	f32 f2 = f5.a + f3.b; // ps_sum0 f2, f5, f3, f3
	double e, c;
	FRSQRTE(f2, e, c);
	if (-f2 >= 0.0f) // fneg f1, f2; fsel f0, f1, f2, f0
		return f2 * f2;
	return MULS_EST(f2, c); // fmuls f1, f2, f0
}

// v2 = v1 times the frsqrte estimate of |v1|² (no Newton step).
static void gMsVECNormalize(const f32* v1, f32* v2)
{
	ps f6  = L(v1);
	ps f3  = mul(f6, f6);
	ps f4  = F(v1[2]);
	ps f5  = madd(f4, f4, f3);
	f32 f2 = f5.a + f3.b; // ps_sum0 f2, f5, f3, f3
	double e, c;
	FRSQRTE(f2, e, c);
	f6 = ps { MULS_EST(f6.a, c), MULS_EST(f6.b, c) }; // ps_muls0 f6, f6, f0
	S(v2, f6);
	v2[2] = MULS_EST(f4.a, c); // fmuls f4, f4, f0
}
