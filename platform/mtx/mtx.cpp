// MTX/VEC: the GameCube's matrix library, computed as the console computes it.
// Matrices are row-major 3x4 (implicit last row 0 0 0 1) acting on column
// vectors, as in the SDK; projection matrices follow the GX clip-space
// convention (z in [-w, 0]).
//
// The DOL links the paired-single routines of the SDK's mtx.c, mtxvec.c and
// vec.c (PSMTXConcat, PSMTXMultVec, PSVECNormalize, ...) and the C routines
// that have no paired-single version (C_MTXLookAt, C_MTXPerspective,
// C_MTXOrtho and the three light-projection routines). mtx_ps.inc follows
// their machine code instruction for instruction, in single precision:
// - every operation rounds to f32, and a fused multiply-add (ps_madd,
//   fmadds, ...) rounds once: fmaf, the host's FMA instruction where the CPU
//   has one (x86 needs a runtime check) and port_fmas_soft (port_fma.h)
//   otherwise;
// - frsqrte and fres are the Gekko's estimates (port_fpu.h), and a
//   single-precision multiply rounds its frC operand to 25 significant bits
//   first, as Dolphin models the Gekko; only a frsqrte estimate is ever
//   wider than that here (the Newton step of PSVECNormalize, PSVECMag and
//   PSVECDistance);
// - sinf, cosf and tanf are MSL's (platform/misc/msl_math.c).
// It is compiled with SSE maths and no contraction on every x86 host
// (CMakeLists.txt), so the 32- and 64-bit builds compute the same bits.
// tools/mtxmath/check.sh checks it against the DOL's own objects.
//
// jsys_ps.inc does the same for the paired-single routines outside the SDK
// library (J3DTransform's J3DPSCalcInverseTranspose and matrix concatenations,
// J3DPSMulMtxVec, J3DModel's weighted envelopes, J3DHermiteInterpolationS,
// MsVECMag2 and MsVECNormalize), which the decomp's host builds call through
// src/port_ps.h (decomp-patches/fpu-01-paired-single-routines).
//
// The C_ names of routines the DOL has only as paired-single code are not in
// the DOL at all (the game's MTXConcat etc. are the PS ones): they forward to
// the PS routine. PSMTXTranspose, PSVECSquareMag and C_MTXFrustum are not in
// the DOL either, and nothing calls them.
#include "port_compat.h"
#include <dolphin/mtx.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "port_fpu.h"
#include "port_fma.h"

extern "C" {
void PSMTXMultVecSR(Mtx m, Vec* src, Vec* dst);
void C_MTXMultVecSR(Mtx m, Vec* src, Vec* dst);
}

// Dolphin's Force25Bit: the frC operand of a single-precision multiply,
// rounded to 25 significant bits (half away from zero).
static inline double force25(double d)
{
	unsigned long long u;
	memcpy(&u, &d, 8);
	u = (u & 0xFFFFFFFFF8000000ull) + (u & 0x8000000ull);
	memcpy(&d, &u, 8);
	return d;
}

// frsqrte of a single (port_gekko_frsqrte), and the estimate as the frC
// operand of fmuls (force25). For a positive normal single this avoids the
// general version's 64-bit integer work, which is slow in the 32-bit build:
// the estimate's significand is 2^26 + mant (mant below 2^26), scaled by a
// power of two that a float holds, and force25 rounds mant at its bit 1.
static inline void gekko_frsqrte(f32 x, double* est, double* frc)
{
	u32 b;
	memcpy(&b, &x, 4);
	if ((b >> 23) - 1 >= 0xFE) { // zero, subnormal, negative, infinite or NaN
		*est = port_gekko_frsqrte(x);
		*frc = force25(*est);
		return;
	}
	int e    = (int)(b >> 23) - 127, q = e >> 1; // q = floor(e / 2)
	u32 i    = (b >> 8) & 0x7FFF;                // the top 15 mantissa bits
	u32 seg  = (u32)(e & 1) * 16 + (i >> 11);
	u32 mant = port_frsqrte_tab[seg][0] - port_frsqrte_tab[seg][1] * (i & 2047);
	u32 pb   = (u32)(127 - 27 - q) << 23; // 2^(-27 - q): the estimate is 2^(-1 - q) * (1 + mant / 2^26)
	f32 p;
	memcpy(&p, &pb, 4);
	*est = (double)(int)(0x4000000 + mant) * p;
	*frc = (double)(int)(0x4000000 + (mant & ~1u) + (mant & 2u)) * p;
}

// Hides a value from the optimiser (see NFMA in mtx_ps.inc).
#define OPAQUE(x) PORT_OPAQUE(x)

#ifdef MTX_TEST_QEMU
// tools/mtxmath: qemu-ppc's arithmetic, to compare with the DOL's objects run
// there: frsqrte is 1/sqrt in double, fres 1/x in single (but +-0.5 for +-0),
// and fmuls rounds the exact product of its operands once.
static inline f32 qemu_fres(f32 x) { return x == 0.0f ? copysignf(0.5f, x) : 1.0f / x; }
#define FRSQRTE(x, e, c) (e = c = 1.0 / sqrt((double)(x)))
#define FRES(x)          qemu_fres(x)
#define MULS_EST(a, c)   ((f32)((__float128)(a) * (c)))
#else
#define FRSQRTE(x, e, c) gekko_frsqrte(x, &e, &c)
#define FRES(x)          port_gekko_fres((double)(x))
#define MULS_EST(a, c)   ((f32)((a) * (c))) // 27 by 25 bits: exact in double
#endif

#if (defined(__x86_64__) || defined(__i386__)) && !defined(MTX_TEST_QEMU)
namespace mtx_soft {
#define FMA(a, c, b) port_fmas_soft(a, c, b)
#include "mtx_ps.inc"
#include "jsys_ps.inc"
#undef FMA
} // namespace mtx_soft
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("fma"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("fma")
#endif
namespace mtx_fma {
#define FMA(a, c, b) __builtin_fmaf(a, c, b)
#include "mtx_ps.inc"
#include "jsys_ps.inc"
#undef FMA
} // namespace mtx_fma
#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif

// 1 when the CPU has FMA (and the OS saves the AVX state), else 0; tests and
// SMS_MTX_SOFT_FMA=1 can force the software path.
extern "C" {
int sms_mtx_hw_fma = -1;
}
static int mtx_init_fma()
{
	const char* e = getenv("SMS_MTX_SOFT_FMA");
	__builtin_cpu_init();
	sms_mtx_hw_fma = (e && *e == '1') ? 0 : __builtin_cpu_supports("fma") ? 1 : 0;
	return sms_mtx_hw_fma;
}
static inline bool mtx_have_fma()
{
	int v = sms_mtx_hw_fma;
	return __builtin_expect(v >= 0, 1) ? v : mtx_init_fma();
}
#define CALL(fn, args) (mtx_have_fma() ? mtx_fma::fn args : mtx_soft::fn args)
#else
// Other hosts (on arm64 fmaf is an instruction), and the qemu test build.
namespace mtx_host {
#define FMA(a, c, b) fmaf(a, c, b)
#include "mtx_ps.inc"
#include "jsys_ps.inc"
#undef FMA
} // namespace mtx_host
#define CALL(fn, args) (mtx_host::fn args)
#endif

#define DEF(ret, ps, c, fn, params, args)                        \
	extern "C" ret ps params { return (ret)CALL(fn, args); }      \
	extern "C" ret c params { return (ret)CALL(fn, args); }
#define DEF1(ret, name, fn, params, args) \
	extern "C" ret name params { return (ret)CALL(fn, args); }

// --- mtx.c ---
DEF(void, PSMTXIdentity, C_MTXIdentity, gMTXIdentity, (Mtx m), (m))
DEF(void, PSMTXCopy, C_MTXCopy, gMTXCopy, (Mtx s, Mtx d), (s, d))
DEF(void, PSMTXConcat, C_MTXConcat, gMTXConcat, (Mtx a, Mtx b, Mtx ab), (a, b, ab))
DEF(u32, PSMTXInverse, C_MTXInverse, gMTXInverse, (Mtx s, Mtx i), (s, i))
DEF(void, PSMTXTranspose, C_MTXTranspose, gMTXTranspose, (Mtx s, Mtx x), (s, x))
DEF(void, PSMTXRotRad, C_MTXRotRad, gMTXRotRad, (Mtx m, char axis, f32 rad), (m, axis, rad))
DEF(void, PSMTXRotTrig, C_MTXRotTrig, gMTXRotTrig, (Mtx m, char axis, f32 s, f32 c), (m, axis, s, c))
DEF(void, PSMTXRotAxisRad, C_MTXRotAxisRad, gMTXRotAxisRad, (Mtx m, Vec* axis, f32 rad), (m, axis, rad))
DEF(void, PSMTXQuat, C_MTXQuat, gMTXQuat, (Mtx m, Quaternion* q), (m, q))
DEF(void, PSMTXTrans, C_MTXTrans, gMTXTrans, (Mtx m, f32 x, f32 y, f32 z), (m, x, y, z))
DEF(void, PSMTXTransApply, C_MTXTransApply, gMTXTransApply, (Mtx s, Mtx d, f32 x, f32 y, f32 z), (s, d, x, y, z))
DEF(void, PSMTXScale, C_MTXScale, gMTXScale, (Mtx m, f32 x, f32 y, f32 z), (m, x, y, z))
DEF(void, PSMTXScaleApply, C_MTXScaleApply, gMTXScaleApply, (Mtx s, Mtx d, f32 x, f32 y, f32 z), (s, d, x, y, z))
DEF1(void, C_MTXLookAt, gMTXLookAt, (Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target), (m, camPos, camUp, target))
DEF1(void, C_MTXLightFrustum, gMTXLightFrustum,
     (Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 sS, f32 sT, f32 tS, f32 tT), (m, t, b, l, r, n, sS, sT, tS, tT))
DEF1(void, C_MTXLightPerspective, gMTXLightPerspective, (Mtx m, f32 fovY, f32 aspect, f32 sS, f32 sT, f32 tS, f32 tT),
     (m, fovY, aspect, sS, sT, tS, tT))
DEF1(void, C_MTXLightOrtho, gMTXLightOrtho, (Mtx m, f32 t, f32 b, f32 l, f32 r, f32 sS, f32 sT, f32 tS, f32 tT),
     (m, t, b, l, r, sS, sT, tS, tT))

// --- mtx44.c ---
DEF1(void, C_MTXPerspective, gMTXPerspective, (Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f), (m, fovY, aspect, n, f))
DEF1(void, C_MTXFrustum, gMTXFrustum, (Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f), (m, t, b, l, r, n, f))
DEF1(void, C_MTXOrtho, gMTXOrtho, (Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f), (m, t, b, l, r, n, f))

// --- mtxvec.c ---
DEF(void, PSMTXMultVec, C_MTXMultVec, gMTXMultVec, (Mtx44 m, Vec* s, Vec* d), (m, s, d))
DEF(void, PSMTXMultVecSR, C_MTXMultVecSR, gMTXMultVecSR, (Mtx m, Vec* s, Vec* d), (m, s, d))
DEF(void, PSMTXMultVecArray, C_MTXMultVecArray, gMTXMultVecArray, (Mtx m, Vec* s, Vec* d, u32 n), (m, s, d, n))

// --- vec.c ---
DEF(void, PSVECAdd, C_VECAdd, gVECAdd, (Vec* a, Vec* b, Vec* c), (a, b, c))
DEF(void, PSVECSubtract, C_VECSubtract, gVECSubtract, (Vec* a, Vec* b, Vec* c), (a, b, c))
DEF(void, PSVECScale, C_VECScale, gVECScale, (Vec* s, Vec* d, f32 k), (s, d, k))
DEF(void, PSVECNormalize, C_VECNormalize, gVECNormalize, (Vec* s, Vec* d), (s, d))
DEF(f32, PSVECSquareMag, C_VECSquareMag, gVECSquareMag, (Vec* v), (v))
DEF(f32, PSVECMag, C_VECMag, gVECMag, (Vec* v), (v))
DEF(f32, PSVECDotProduct, C_VECDotProduct, gVECDotProduct, (Vec* a, Vec* b), (a, b))
DEF(void, PSVECCrossProduct, C_VECCrossProduct, gVECCrossProduct, (Vec* a, Vec* b, Vec* c), (a, b, c))
DEF(f32, PSVECSquareDistance, C_VECSquareDistance, gVECSquareDistance, (Vec* a, Vec* b), (a, b))
DEF(f32, PSVECDistance, C_VECDistance, gVECDistance, (Vec* a, Vec* b), (a, b))

// --- JSystem's and the game's paired-single routines (jsys_ps.inc), called
// by the decomp's host builds (decomp-patches/fpu-01-paired-single-routines) ---
#include "port_ps.h"
DEF1(int, port_J3DPSCalcInverseTranspose, gJ3DPSCalcInverseTranspose, (const f32 (*src)[4], f32 (*dst)[3]), (src, dst))
DEF1(void, port_J3DMtxProjConcat, gJ3DMtxProjConcat, (const f32 (*a)[4], const f32 (*b)[4], f32 (*ab)[4]), (a, b, ab))
DEF1(void, port_J3DMTXConcatArrayIndexedSrc, gJ3DMTXConcatArrayIndexedSrc,
     (const f32 (*a)[4], const f32 (*b)[3][4], const u16* idx, f32 (*dst)[3][4], u32 n), (a, b, idx, dst, n))
DEF1(void, port_J3DPSMtxArrayConcat, gJ3DPSMtxArrayConcat, (const f32 (*a)[4], const f32 (*b)[4], f32 (*dst)[4], u32 n),
     (a, b, dst, n))
DEF1(void, port_J3DPSMulMtxVec, gJ3DPSMulMtxVec, (const f32 (*m)[4], const f32* v, f32* d), (m, v, d))
DEF1(void, port_J3DPSMulMtxVec33, gJ3DPSMulMtxVec33, (const f32 (*m)[3], const f32* v, f32* d), (m, v, d))
DEF1(void, port_J3DWeightEnvelopeMix, gJ3DWeightEnvelopeMix,
     (f32 (*acc)[4], const f32 (*world)[4], const f32 (*inv)[4], f32 weight), (acc, world, inv, weight))
DEF1(f32, port_J3DHermiteInterpolationS, gJ3DHermiteInterpolationS,
     (f32 t, f32 time0, f32 value0, f32 tangent0, f32 time1, f32 value1, f32 tangent1),
     (t, time0, value0, tangent0, time1, value1, tangent1))
DEF1(f32, port_MsVECMag2, gMsVECMag2, (const f32* v), (v))
DEF1(void, port_MsVECNormalize, gMsVECNormalize, (const f32* v1, f32* v2), (v1, v2))
