// The GameCube's MTX/VEC routines, instruction for instruction (included by
// mtx.cpp, once per fused multiply-add implementation).
//
// Each function follows the DOL's machine code (the decomp's split of the
// original objects, build/GMSE01/obj/dolphin/mtx): the paired-single routines
// of mtx.c, mtxvec.c and vec.c, and the C routines of mtx.c and mtx44.c as
// MWCC compiled them (-fp_contract off, so no fused operations there).
// A paired-single register is a `ps` (ps0, ps1). Every operation rounds to
// single precision as the Gekko does: products and sums once each, fused
// multiply-adds once in all (FMA, defined by the includer). Registers are
// named after the ones the DOL uses, so the code can be read against the
// disassembly; halves that are never stored are computed all the same and
// left to the compiler.
//
// Before inclusion: FMA(a, c, b) = a * c + b rounded once to single, and the
// frsqrte/fres models (FRSQRTE, FRES, MULS_EST: mtx.cpp).

// -(a * c + b), the negation after the rounding: GCC would otherwise fold
// -fma(a, c, -b) into fnmadd, -(a * c) + b, which differs in the sign of a zero
// result (fnmsubs gives -0 where a * c == b).
static inline f32 NFMA(f32 a, f32 c, f32 b)
{
	f32 r = FMA(a, c, b);
	OPAQUE(r);
	return -r;
}

struct ps {
	f32 a, b; // ps0, ps1
};

static inline ps L(const f32* p) { return ps { p[0], p[1] }; }     // psq_l, W=0
static inline ps L1(const f32* p) { return ps { p[0], 1.0f }; }    // psq_l, W=1
static inline void S(f32* p, ps v) { p[0] = v.a, p[1] = v.b; }     // psq_st, W=0
static inline void S1(f32* p, ps v) { p[0] = v.a; }                // psq_st, W=1
static inline ps F(f32 v) { return ps { v, v }; } // lfs and single-precision scalar results fill both halves

static inline ps add(ps a, ps b) { return ps { a.a + b.a, a.b + b.b }; }
static inline ps sub(ps a, ps b) { return ps { a.a - b.a, a.b - b.b }; }
static inline ps neg(ps a) // after rounding, as NFMA
{
	OPAQUE(a.a);
	OPAQUE(a.b);
	return ps { -a.a, -a.b };
}
static inline ps mul(ps a, ps c) { return ps { a.a * c.a, a.b * c.b }; }
static inline ps muls0(ps a, ps c) { return ps { a.a * c.a, a.b * c.a }; }
static inline ps muls1(ps a, ps c) { return ps { a.a * c.b, a.b * c.b }; }
static inline ps madd(ps a, ps c, ps b) { return ps { FMA(a.a, c.a, b.a), FMA(a.b, c.b, b.b) }; }
static inline ps madds0(ps a, ps c, ps b) { return ps { FMA(a.a, c.a, b.a), FMA(a.b, c.a, b.b) }; }
static inline ps madds1(ps a, ps c, ps b) { return ps { FMA(a.a, c.b, b.a), FMA(a.b, c.b, b.b) }; }
static inline ps msub(ps a, ps c, ps b) { return ps { FMA(a.a, c.a, -b.a), FMA(a.b, c.b, -b.b) }; }
static inline ps nmadd(ps a, ps c, ps b) { return ps { NFMA(a.a, c.a, b.a), NFMA(a.b, c.b, b.b) }; }
static inline ps nmsub(ps a, ps c, ps b) { return ps { NFMA(a.a, c.a, -b.a), NFMA(a.b, c.b, -b.b) }; }
static inline ps sum0(ps a, ps c, ps b) { return ps { a.a + b.b, c.b }; }
static inline ps sum1(ps a, ps c, ps b) { return ps { c.a, a.a + b.b }; }
static inline ps m00(ps a, ps b) { return ps { a.a, b.a }; }
static inline ps m01(ps a, ps b) { return ps { a.a, b.b }; }
static inline ps m10(ps a, ps b) { return ps { a.b, b.a }; }
static inline ps m11(ps a, ps b) { return ps { a.b, b.b }; }

// The Newton-Raphson step of PSVECNormalize, PSVECMag and PSVECDistance:
// frsqrte e; fmuls e*e; fmuls e*0.5; fnmsubs 3 - sq*e*e; fmuls.
static inline f32 rsqrt_step(f32 sq)
{
	double e, c; // the estimate, and the estimate as fmuls's frC operand
	FRSQRTE(sq, e, c);
	f32 ee   = MULS_EST(e, c);
	f32 half = (f32)(e * 0.5);
	f32 t    = NFMA(ee, sq, -3.0f);
	return t * half;
}
// fsel r, r, r, sq; fmuls sq * r: the square root, and 0 for 0.
static inline f32 sqrt_tail(f32 sq, f32 r) { return sq * (r >= 0.0f ? r : sq); }

// --- mtx.c ---
static void gMTXIdentity(Mtx m)
{
	ps f0 = F(0.0f), f1 = F(1.0f);
	S(&m[0][2], f0);
	ps f2 = m01(f0, f1);
	S(&m[1][2], f0);
	f1 = m10(f1, f0);
	S(&m[2][0], f0);
	S(&m[1][0], f2);
	S(&m[0][0], f1);
	S(&m[2][2], f1);
}
static void gMTXCopy(Mtx src, Mtx dst)
{
	const f32* s = &src[0][0];
	f32* d       = &dst[0][0];
	for (int i = 0; i < 12; i += 2)
		S(d + i, L(s + i));
}
static void gMTXConcat(Mtx a, Mtx b, Mtx ab)
{
	ps f0 = L(&a[0][0]), f1 = L(&a[0][2]), f2 = L(&a[1][0]), f3 = L(&a[1][2]), f4 = L(&a[2][0]), f5 = L(&a[2][2]);
	ps f6 = L(&b[0][0]), f7 = L(&b[0][2]), f8 = L(&b[1][0]), f9 = L(&b[1][2]), f10 = L(&b[2][0]), f11 = L(&b[2][2]);
	ps f31 = ps { 0.0f, 1.0f }; // Unit01
	ps f12 = muls0(f6, f0), f13 = muls0(f7, f0), f14 = muls0(f6, f2), f15 = muls0(f7, f2);
	f12 = madds1(f8, f0, f12);
	f14 = madds1(f8, f2, f14);
	f13 = madds1(f9, f0, f13);
	f15 = madds1(f9, f2, f15);
	f12 = madds0(f10, f1, f12);
	f13 = madds0(f11, f1, f13);
	f14 = madds0(f10, f3, f14);
	f15 = madds0(f11, f3, f15);
	S(&ab[0][0], f12);
	f2  = muls0(f6, f4);
	f13 = madds1(f31, f1, f13);
	f0  = muls0(f7, f4);
	S(&ab[1][0], f14);
	f15 = madds1(f31, f3, f15);
	S(&ab[0][2], f13);
	f2 = madds1(f8, f4, f2);
	f0 = madds1(f9, f4, f0);
	f2 = madds0(f10, f5, f2);
	S(&ab[1][2], f15);
	f0 = madds0(f11, f5, f0);
	S(&ab[2][0], f2);
	f0 = madds1(f31, f5, f0);
	S(&ab[2][2], f0);
}
static u32 gMTXInverse(Mtx src, Mtx inv)
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
	ps f13 = mul(f5, f7);
	ps f8  = m10(f5, f4);
	f11    = msub(f1, f7, f11);
	ps f12 = mul(f1, f8);
	f13    = msub(f3, f8, f13);
	ps f10 = mul(f3, f4);
	f12    = msub(f5, f6, f12);
	ps f9  = mul(f0, f5);
	f8     = mul(f1, f2);
	f6     = sub(f6, f6);
	f10    = msub(f2, f5, f10);
	f7     = mul(f0, f13);
	f9     = msub(f1, f4, f9);
	f7     = madd(f2, f12, f7);
	f8     = msub(f0, f3, f8);
	f7     = madd(f4, f11, f7);
	if (f7.a == f6.a) // ps_cmpo0: the determinant against 0 (NaN for an infinite src[0][2])
		return 0;
	f0 = F(FRES(f7.a));
	f6 = add(f0, f0);
	f5 = mul(f0, f0);
	f0 = nmsub(f7, f5, f6);
	f1 = F(src[0][3]);
	f13 = muls0(f13, f0);
	f2 = F(src[1][3]);
	f12 = muls0(f12, f0);
	f3 = F(src[2][3]);
	f11 = muls0(f11, f0);
	f5  = m00(f13, f12);
	f10 = muls0(f10, f0);
	f4  = m11(f13, f12);
	f9  = muls0(f9, f0);
	S(&inv[0][0], f5);
	f6 = mul(f13, f1);
	S(&inv[1][0], f4);
	f8 = muls0(f8, f0);
	f6 = madd(f12, f2, f6);
	S1(&inv[2][0], f10);
	f6 = nmadd(f11, f3, f6);
	S1(&inv[2][1], f9);
	f7 = mul(f10, f1);
	f5 = m00(f11, f6);
	S1(&inv[2][2], f8);
	f4 = m11(f11, f6);
	S(&inv[0][2], f5);
	f7 = madd(f9, f2, f7);
	S(&inv[1][2], f4);
	f7 = nmadd(f8, f3, f7);
	S1(&inv[2][3], f7);
	return 1;
}
static void gMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
	ps f0 = F(0.0f), f3 = F(1.0f), f1 = F(sinA), f2 = F(cosA);
	ps f4 = neg(f1), f5;
	switch ((u8)axis | 0x20) {
	case 'x':
		S1(&m[0][0], f3);
		S(&m[0][1], f0);
		f5 = m00(f1, f2);
		S(&m[0][3], f0);
		f2 = m00(f2, f4);
		S(&m[1][3], f0);
		S1(&m[2][3], f0);
		S(&m[2][1], f5);
		S(&m[1][1], f2);
		break;
	case 'y':
		f5 = m00(f2, f0);
		f2 = m00(f0, f3);
		S(&m[1][2], f0);
		S(&m[0][0], f5);
		f4 = m00(f4, f0);
		f0 = m00(f1, f0);
		S(&m[2][2], f5);
		S(&m[1][0], f2);
		S(&m[0][2], f0);
		S(&m[2][0], f4);
		break;
	case 'z':
		S(&m[0][2], f0);
		f5 = m00(f1, f2);
		f4 = m00(f2, f4);
		S(&m[1][2], f0);
		S(&m[2][0], f0);
		f2 = m00(f3, f0);
		S(&m[1][0], f5);
		S(&m[0][0], f4);
		S(&m[2][2], f2);
		break;
	}
}
static void gMTXRotRad(Mtx m, char axis, f32 rad)
{
	f32 s = sms_msl_sinf(rad);
	f32 c = sms_msl_cosf(rad);
	gMTXRotTrig(m, axis, s, c);
}
static void gVECNormalize(Vec* src, Vec* unit);
static void gMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
	ps f28 = F(0.0f);
	f32 s  = sms_msl_sinf(rad);
	f32 c  = sms_msl_cosf(rad);
	ps f29 = F(1.0f - c);
	Vec v;
	gVECNormalize(axis, &v);
	ps f27 = L(&v.x);
	ps f1  = F(v.z);
	ps f0  = F(c); // ps_merge00 f0, f31, f31
	ps f4  = muls0(f27, f29);
	ps f5  = muls0(f1, f29);
	ps f3  = muls1(f4, f27);
	ps f2  = muls0(f4, f27);
	f27    = muls0(f27, F(s));
	f4     = muls0(f4, f1);
	ps f6  = F(NFMA(f1.a, s, -f3.a)); // fnmsubs
	ps f7  = F(FMA(f1.a, s, f3.a));   // fmadds
	ps f9  = neg(f27);
	ps f8  = sum0(f4, f28, f27);
	f2     = sum0(f2, f6, f0);
	f3     = sum1(f0, f7, f3);
	f6     = sum0(f9, f28, f4);
	f9     = sum0(f4, f4, f9);
	S(&m[0][2], f8);
	f5 = muls0(f5, f1);
	S(&m[0][0], f2);
	f4 = sum1(f27, f9, f4);
	S(&m[1][0], f3);
	f5 = sum0(f5, f28, f0);
	S(&m[1][2], f6);
	S(&m[2][0], f4);
	S(&m[2][2], f5);
}
static void gMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
	ps f0 = F(0.0f);
	f32 one = 1.0f;
	m[0][3] = xT;
	m[1][3] = yT;
	S(&m[0][1], f0);
	S(&m[2][0], f0);
	m[1][0] = 0.0f;
	m[1][1] = one;
	m[1][2] = 0.0f;
	m[2][2] = one;
	m[2][3] = zT;
	m[0][0] = one;
}
static void gMTXTransApply(Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT)
{
	ps f4 = L(&src[0][0]), f5 = L(&src[0][2]), f7 = L(&src[1][2]), f8 = L(&src[2][2]);
	f5    = sum1(F(xT), f5, f5);
	ps f6 = L(&src[1][0]);
	f7    = sum1(F(yT), f7, f7);
	ps f9 = L(&src[2][0]);
	f8    = sum1(F(zT), f8, f8);
	S(&dst[0][0], f4);
	S(&dst[0][2], f5);
	S(&dst[1][0], f6);
	S(&dst[1][2], f7);
	S(&dst[2][0], f9);
	S(&dst[2][2], f8);
}
static void gMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
	ps f0   = F(0.0f);
	m[0][0] = xS;
	S(&m[0][1], f0);
	S(&m[0][3], f0);
	m[1][1] = yS;
	S(&m[1][2], f0);
	S(&m[2][0], f0);
	m[2][2] = zS;
	m[2][3] = 0.0f;
}
static void gMTXScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS)
{
	ps x = F(xS), y = F(yS), z = F(zS);
	ps f4 = L(&src[0][0]), f5 = L(&src[0][2]);
	f4    = muls0(f4, x);
	ps f6 = L(&src[1][0]);
	f5    = muls0(f5, x);
	ps f7 = L(&src[1][2]);
	f6    = muls0(f6, y);
	ps f8 = L(&src[2][0]);
	S(&dst[0][0], f4);
	f7    = muls0(f7, y);
	ps f2 = L(&src[2][2]);
	S(&dst[0][2], f5);
	f8 = muls0(f8, z);
	S(&dst[1][0], f6);
	f2 = muls0(f2, z);
	S(&dst[1][2], f7);
	S(&dst[2][0], f8);
	S(&dst[2][2], f2);
}
static void gMTXQuat(Mtx m, Quaternion* q)
{
	ps f1  = F(1.0f);
	ps f4  = L(&q->x), f5 = L(&q->z);
	ps f0  = F(1.0f - 1.0f);
	ps f2  = F(1.0f + 1.0f);
	ps f6  = mul(f4, f4);
	ps f9  = m10(f4, f4);
	ps f8  = madd(f5, f5, f6);
	ps f7  = mul(f5, f5);
	ps f3  = sum0(f8, f8, f8);
	ps f10 = muls1(f9, f5);
	ps f11 = F(FRES(f3.a));
	f8     = sum1(f7, f8, f6);
	f3     = nmsub(f3, f11, f2);
	f7     = muls1(f5, f5);
	f3     = mul(f11, f3);
	f6     = sum0(f6, f6, f6);
	f3     = F(f3.a * f2.a); // fmuls
	f11    = madd(f4, f9, f7);
	f7     = msub(f4, f9, f7);
	S1(&m[0][3], f0);
	f6 = nmsub(f6, f3, f1);
	f8 = nmsub(f8, f3, f1);
	S1(&m[2][3], f0);
	f11 = mul(f11, f3);
	f7  = mul(f7, f3);
	S1(&m[2][2], f6);
	f9  = madds0(f4, f5, f10);
	f5  = m00(f11, f8);
	f10 = nmsub(f10, f2, f9);
	f4  = m10(f8, f7);
	S(&m[1][0], f5);
	f9  = mul(f9, f3);
	f10 = mul(f10, f3);
	S(&m[0][0], f4);
	S1(&m[0][2], f9);
	f7  = m10(f10, f0);
	f11 = m01(f10, f9);
	S(&m[1][2], f7);
	S(&m[2][0], f11);
}
static void gVECCrossProduct(Vec* a, Vec* b, Vec* axb);
static void gMTXLookAt(Mtx m, Vec* camPos, Vec* camUp, Vec* target)
{
	Vec look, right, up;
	look.x = camPos->x - target->x;
	look.y = camPos->y - target->y;
	look.z = camPos->z - target->z;
	gVECNormalize(&look, &look);
	gVECCrossProduct(camUp, &look, &right);
	gVECNormalize(&right, &right);
	gVECCrossProduct(&look, &right, &up);
	m[0][0] = right.x;
	m[0][1] = right.y;
	m[0][2] = right.z;
	m[0][3] = -(camPos->z * right.z + (camPos->x * right.x + camPos->y * right.y));
	m[1][0] = up.x;
	m[1][1] = up.y;
	m[1][2] = up.z;
	m[1][3] = -(camPos->z * up.z + (camPos->x * up.x + camPos->y * up.y));
	m[2][0] = look.x;
	m[2][1] = look.y;
	m[2][2] = look.z;
	m[2][3] = -(camPos->z * look.z + (camPos->x * look.x + camPos->y * look.y));
}
static void gMTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 sS, f32 sT, f32 tS, f32 tT)
{
	f32 rl = 1.0f / (r - l), tb = 1.0f / (t - b), n2 = 2.0f * n;
	m[0][0] = sS * (n2 * rl);
	m[0][1] = 0.0f;
	m[0][2] = sS * (rl * (r + l)) - tS;
	m[0][3] = 0.0f;
	m[1][0] = 0.0f;
	m[1][1] = sT * (n2 * tb);
	m[1][2] = sT * (tb * (t + b)) - tT;
	m[1][3] = 0.0f;
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = -1.0f;
	m[2][3] = 0.0f;
}
// fovY * 0.5f * (pi / 180): the DOL's constants
static inline f32 half_fov_rad(f32 fovY) { return 0.017453292f * (0.5f * fovY); }
static void gMTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 sS, f32 sT, f32 tS, f32 tT)
{
	f32 cot = 1.0f / sms_msl_tanf(half_fov_rad(fovY));
	m[0][0] = sS * (cot / aspect);
	m[0][1] = 0.0f;
	m[0][2] = -tS;
	m[0][3] = 0.0f;
	m[1][0] = 0.0f;
	m[1][1] = cot * sT;
	m[1][2] = -tT;
	m[1][3] = 0.0f;
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = -1.0f;
	m[2][3] = 0.0f;
}
static void gMTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 sS, f32 sT, f32 tS, f32 tT)
{
	f32 rl = 1.0f / (r - l), tb = 1.0f / (t - b);
	m[0][0] = (2.0f * rl) * sS;
	m[0][1] = 0.0f;
	m[0][2] = 0.0f;
	m[0][3] = tS + sS * (rl * -(r + l));
	m[1][0] = 0.0f;
	m[1][1] = (2.0f * tb) * sT;
	m[1][2] = 0.0f;
	m[1][3] = tT + sT * (tb * -(t + b));
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = 0.0f;
	m[2][3] = 1.0f;
}

// --- mtx44.c ---
static void gMTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)
{
	f32 cot = 1.0f / sms_msl_tanf(half_fov_rad(fovY));
	f32 fn  = 1.0f / (f - n);
	m[0][0] = cot / aspect;
	m[0][1] = 0.0f;
	m[0][2] = 0.0f;
	m[0][3] = 0.0f;
	m[1][0] = 0.0f;
	m[1][1] = cot;
	m[1][2] = 0.0f;
	m[1][3] = 0.0f;
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = -n * fn;
	m[2][3] = fn * -(f * n);
	m[3][0] = 0.0f;
	m[3][1] = 0.0f;
	m[3][2] = -1.0f;
	m[3][3] = 0.0f;
}
static void gMTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	f32 rl = 1.0f / (r - l), tb = 1.0f / (t - b), fn = 1.0f / (f - n);
	m[0][0] = 2.0f * rl;
	m[0][1] = 0.0f;
	m[0][2] = 0.0f;
	m[0][3] = rl * -(r + l);
	m[1][0] = 0.0f;
	m[1][1] = 2.0f * tb;
	m[1][2] = 0.0f;
	m[1][3] = tb * -(t + b);
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = -1.0f * fn;
	m[2][3] = -f * fn;
	m[3][0] = 0.0f;
	m[3][1] = 0.0f;
	m[3][2] = 0.0f;
	m[3][3] = 1.0f;
}
// Not in the DOL: the decomp's MWCC build of mtx44.c.
static void gMTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	f32 rl = 1.0f / (r - l), tb = 1.0f / (t - b), fn = 1.0f / (f - n), n2 = 2.0f * n;
	m[0][0] = n2 * rl;
	m[0][1] = 0.0f;
	m[0][2] = rl * (r + l);
	m[0][3] = 0.0f;
	m[1][0] = 0.0f;
	m[1][1] = n2 * tb;
	m[1][2] = tb * (t + b);
	m[1][3] = 0.0f;
	m[2][0] = 0.0f;
	m[2][1] = 0.0f;
	m[2][2] = -n * fn;
	m[2][3] = fn * -(f * n);
	m[3][0] = 0.0f;
	m[3][1] = 0.0f;
	m[3][2] = -1.0f;
	m[3][3] = 0.0f;
}

// --- mtxvec.c ---
static void gMTXMultVec(Mtx m, Vec* src, Vec* dst)
{
	ps f0 = L(&src->x), f1 = L1(&src->z);
	ps f2 = L(&m[0][0]);
	ps f4 = mul(f2, f0);
	ps f3 = L(&m[0][2]);
	ps f5 = madd(f3, f1, f4);
	ps f8 = L(&m[1][0]);
	f32 x = f5.a + f5.b; // ps_sum0
	ps f9  = L(&m[1][2]);
	ps f10 = mul(f8, f0);
	dst->x = x;
	ps f11 = madd(f9, f1, f10);
	f2     = L(&m[2][0]);
	f32 y  = f11.a + f11.b;
	f3     = L(&m[2][2]);
	f4     = mul(f2, f0);
	dst->y = y;
	f5     = madd(f3, f1, f4);
	dst->z = f5.a + f5.b;
}
static void gMTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count)
{
	// The DOL's loop runs count - 1 times with the last vector after it, so a
	// count of 0 or 1 runs it 2^32 times there; here they convert 0 and 1 vectors.
	if (count == 0)
		return;
	ps f13 = L(&m[0][0]), f12 = L(&m[1][0]), f11 = L(&m[0][2]), f10 = L(&m[1][2]);
	ps f0 = m00(f13, f12), f1 = m11(f13, f12), f2 = m00(f11, f10), f3 = m11(f11, f10);
	ps f4 = L(&m[2][0]), f5 = L(&m[2][2]);
	ps f6 = L(&srcBase->x), f7 = L1(&srcBase->z);
	ps f8 = madds0(f0, f6, f3), f9 = mul(f4, f6);
	f8    = madds1(f1, f6, f8);
	f10   = madd(f5, f7, f9);
	for (u32 i = 0;; i++) {
		f12 = madds0(f2, f7, f8);
		f13 = sum0(f10, f9, f10);
		if (i + 1 < count) { // the next vector is loaded before this one is stored
			f6 = L(&srcBase[i + 1].x);
			f7 = L1(&srcBase[i + 1].z);
			f8 = madds0(f0, f6, f3);
			f9 = mul(f4, f6);
		}
		S(&dstBase[i].x, f12);
		S1(&dstBase[i].z, f13);
		if (i + 1 >= count)
			break;
		f8  = madds1(f1, f6, f8);
		f10 = madd(f5, f7, f9);
	}
}
static void gMTXMultVecSR(Mtx m, Vec* src, Vec* dst)
{
	ps f0 = L(&m[0][0]), f6 = L(&src->x), f2 = L(&m[1][0]);
	ps f8 = mul(f0, f6);
	ps f4 = L(&m[2][0]);
	ps f10 = mul(f2, f6);
	ps f7  = L1(&src->z);
	ps f12 = mul(f4, f6);
	ps f3  = L(&m[1][2]);
	f8     = sum0(f8, f8, f8);
	ps f5  = L(&m[2][2]);
	f10    = sum0(f10, f10, f10);
	ps f1  = L(&m[0][2]);
	f12    = sum0(f12, f12, f12);
	dst->x = FMA(f1.a, f7.a, f8.a);
	dst->y = FMA(f3.a, f7.a, f10.a);
	dst->z = FMA(f5.a, f7.a, f12.a);
}

// --- vec.c ---
static void gVECAdd(Vec* a, Vec* b, Vec* c)
{
	S(&c->x, add(L(&a->x), L(&b->x)));
	S1(&c->z, add(L1(&a->z), L1(&b->z)));
}
static void gVECSubtract(Vec* a, Vec* b, Vec* c)
{
	S(&c->x, sub(L(&a->x), L(&b->x)));
	S1(&c->z, sub(L1(&a->z), L1(&b->z)));
}
static void gVECScale(Vec* src, Vec* dst, f32 scale)
{
	ps f0 = L(&src->x), f2 = L1(&src->z), f1 = F(scale);
	S(&dst->x, muls0(f0, f1));
	S1(&dst->z, muls0(f2, f1));
}
static void gVECNormalize(Vec* src, Vec* unit)
{
	ps f2 = L(&src->x);
	ps f5 = mul(f2, f2);
	ps f3 = L1(&src->z);
	ps f4 = madd(f3, f3, f5);
	f4    = sum0(f4, f3, f5);
	ps r  = F(rsqrt_step(f4.a));
	S(&unit->x, muls0(f2, r));
	S1(&unit->z, muls0(f3, r));
}
// Not in the DOL: PSVECMag's sum of squares.
static f32 gVECSquareMag(Vec* v)
{
	ps f0 = L(&v->x);
	f0    = mul(f0, f0);
	ps f1 = F(v->z);
	f1    = madd(f1, f1, f0);
	return f1.a + f0.b;
}
static f32 gVECMag(Vec* v)
{
	f32 sq = gVECSquareMag(v);
	return sqrt_tail(sq, rsqrt_step(sq));
}
static f32 gVECDotProduct(Vec* a, Vec* b)
{
	ps f2 = L(&a->y), f3 = L(&b->y);
	f2    = mul(f2, f3);
	ps f5 = L(&a->x), f4 = L(&b->x);
	f3    = madd(f5, f4, f2);
	return f3.a + f2.b;
}
static void gVECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
	ps f1  = L(&b->x);
	ps f2  = F(a->z);
	ps f0  = L(&a->x);
	ps f6  = m10(f1, f1);
	ps f3  = F(b->z);
	ps f4  = mul(f1, f2);
	ps f7  = muls0(f1, f0);
	ps f5  = msub(f0, f3, f4);
	ps f8  = msub(f0, f6, f7);
	ps f9  = m11(f5, f5);
	ps f10 = m01(f5, f8);
	S1(&axb->x, f9);
	f10 = neg(f10);
	S(&axb->y, f10);
}
static f32 gVECSquareDistance(Vec* a, Vec* b)
{
	ps f2 = sub(L(&a->y), L(&b->y));
	ps f0 = L(&a->x), f1 = L(&b->x);
	f2    = mul(f2, f2);
	f0    = sub(f0, f1);
	f1    = madd(f0, f0, f2);
	return f1.a + f2.b;
}
static f32 gVECDistance(Vec* a, Vec* b)
{
	f32 sq = gVECSquareDistance(a, b);
	return sqrt_tail(sq, rsqrt_step(sq));
}
// Not in the DOL: moves only.
static void gMTXTranspose(Mtx src, Mtx xPose)
{
	Mtx t;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			t[i][j] = src[j][i];
	t[0][3] = t[1][3] = t[2][3] = 0.0f;
	memcpy(xPose, t, sizeof(Mtx));
}
