/* The paired-single routines of JSystem and the game outside the SDK's
 * MTX/VEC library, computed as the console computes them
 * (platform/mtx/jsys_ps.inc). The decomp writes them as Gekko assembly for
 * MWCC; decomp-patches/fpu-01-paired-single-routines makes their host builds
 * call these. Vectors are float[3]. */
#ifndef SMS_PORT_PS_H
#define SMS_PORT_PS_H

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif
/* J3DTransform.cpp */
int port_J3DPSCalcInverseTranspose(const f32 (*src)[4], f32 (*dst)[3]);
void port_J3DMtxProjConcat(const f32 (*a)[4], const f32 (*b)[4], f32 (*ab)[4]);
void port_J3DMTXConcatArrayIndexedSrc(const f32 (*a)[4], const f32 (*b)[3][4], const u16* idx, f32 (*dst)[3][4],
                                      u32 n);
void port_J3DPSMtxArrayConcat(const f32 (*a)[4], const f32 (*b)[4], f32 (*dst)[4], u32 n);
/* J3DTransform.hpp: J3DPSMulMtxVec for a 3x4 and a 3x3 matrix */
void port_J3DPSMulMtxVec(const f32 (*m)[4], const f32* v, f32* d);
void port_J3DPSMulMtxVec33(const f32 (*m)[3], const f32* v, f32* d);
/* J3DModel.cpp: acc += weight * (world * inv), one mix matrix of
 * J3DModel::calcWeightEnvelopeMtx */
void port_J3DWeightEnvelopeMix(f32 (*acc)[4], const f32 (*world)[4], const f32 (*inv)[4], f32 weight);
/* J3DAnimation.cpp: J3DHermiteInterpolationS with its s16 keys as floats */
f32 port_J3DHermiteInterpolationS(f32 t, f32 time0, f32 value0, f32 tangent0, f32 time1, f32 value1, f32 tangent1);
/* MathUtil.cpp */
f32 port_MsVECMag2(const f32* v);
void port_MsVECNormalize(const f32* v1, f32* v2);
#ifdef __cplusplus
}
#endif

#endif
