// The float arithmetic of the Dolphin SDK's GX functions that compute
// something (GXLight.c, GXTransform.c, GXDraw.c), as the DOL's objects
// compute it: marioUS.MAP links GXInitLightDistAttn, GXInitSpecularDir,
// GXProject and GXDrawSphere; GXInitLightSpot, GXInitLightDir,
// GXInitSpecularDirHA, GXDrawCylinder, GXDrawSphere1 and GXInitFogAdjTable
// are UNUSED there, and nothing in the game calls them.
//
// MWCC built those objects without contraction (they have no fused
// multiply-add), so each product, sum and quotient here is one single-precision
// operation in the DOL's order, which SSE (or any IEEE single-precision unit)
// rounds as the Gekko does; sms_gx is built with -ffp-contract=off. Where the
// DOL calls MSL (sinf and cosf in GXDrawSphere) these call msl_math.c.
// tools/gxmath/check.sh compares every function with the DOL's objects under
// qemu-ppc and between the 32 and 64-bit builds (docs/64-BIT.md, item 18).
#ifndef SMS_GX_SDK_MATH_H
#define SMS_GX_SDK_MATH_H

#include <math.h>
#include <stdint.h>
#include "msl_math.h"

// GXInitLightDistAttn: k0, k1, k2 for a distance function. The DOL computes
// the same quotients in this order; ref_dist < 0, or ref_br outside (0, 1),
// turns the function off, and a NaN does neither.
static inline void gxsdkLightDistAttn(float refDist, float refBr, int fn, float k[3]) {
    if (refDist < 0.0f) fn = 0;                       // GX_DA_OFF
    if (refBr <= 0.0f || refBr >= 1.0f) fn = 0;
    float k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    switch (fn) {
    case 1:                                           // GX_DA_GENTLE
        k1 = (1.0f - refBr) / (refBr * refDist);
        break;
    case 2:                                           // GX_DA_MEDIUM
        k1 = (0.5f * (1.0f - refBr)) / (refBr * refDist);
        k2 = (0.5f * (1.0f - refBr)) / (refBr * refDist * refDist);
        break;
    case 3:                                           // GX_DA_STEEP
        k2 = (1.0f - refBr) / (refBr * refDist * refDist);
        break;
    default: break;
    }
    k[0] = k0; k[1] = k1; k[2] = k2;
}

// GXInitSpecularDir: the half-angle direction and the light's far position.
// The DOL takes the square root with MSL's inline sqrtf (frsqrte and three
// Newton-Raphson steps in double, unfused here, then frsp), which gives the
// correctly rounded root of every positive finite float (docs/64-BIT.md,
// item 15) and returns x for zero; so the host's sqrtf gives the DOL's
// result for every input whose sum of squares is finite. (Only a sum that
// overflows to +inf, |n| above about 1.8e19, differs: MSL's sequence gives
// a NaN there.)
static inline void gxsdkSpecularDir(float nx, float ny, float nz, float dir[3], float pos[3]) {
    float vx = -nx, vy = -ny, vz = -nz + 1.0f;
    float mag = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
    dir[0] = vx * mag; dir[1] = vy * mag; dir[2] = vz * mag;
    pos[0] = -nx * 1048576.0f; pos[1] = -ny * 1048576.0f; pos[2] = -nz * 1048576.0f;
}

// GXProject: an eye-space point through GXGetProjectionv's and
// GXGetViewportv's values to window coordinates, with the DOL's association
// (x / 2 is a multiplication by 0.5 there, which is exact).
static inline void gxsdkProject(float x, float y, float z, const float mtx[3][4], const float* pm, const float* vp,
                                float* sx, float* sy, float* sz) {
    float px = mtx[0][3] + (mtx[0][2] * z + (mtx[0][0] * x + mtx[0][1] * y));
    float py = mtx[1][3] + (mtx[1][2] * z + (mtx[1][0] * x + mtx[1][1] * y));
    float pz = mtx[2][3] + (mtx[2][2] * z + (mtx[2][0] * x + mtx[2][1] * y));
    float xc, yc, zc, wc;
    if (pm[0] == 0.0f) {
        xc = px * pm[1] + pz * pm[2];
        yc = py * pm[3] + pz * pm[4];
        zc = pm[6] + pz * pm[5];
        wc = 1.0f / -pz;
    } else {
        xc = pm[2] + px * pm[1];
        yc = pm[4] + py * pm[3];
        zc = pm[6] + pz * pm[5];
        wc = 1.0f;
    }
    *sx = vp[2] * 0.5f + (vp[0] + wc * (xc * vp[2] * 0.5f));
    *sy = vp[3] * 0.5f + (vp[1] + wc * (-yc * vp[3] * 0.5f));
    *sz = vp[5] + wc * (zc * (vp[5] - vp[4]));
}

// GXDrawSphere's rings: numMajor triangle strips from the +z pole down, each
// emitting the next ring's vertex before the current one's, positions on the
// unit sphere (the normal is the position divided by the radius, 1.0f, which
// is exact). sinf and cosf are MSL's, as the DOL calls them. The texture
// coordinates the SDK adds when GX_VA_TEX0 is enabled are not generated (the
// game's one call, TSky::perform, clears the vertex descriptor first).
static inline void gxsdkSphere(uint8_t numMajor, uint8_t numMinor, void (*begin)(uint16_t count),
                               void (*vertex)(float x, float y, float z)) {
    const float majorStep = 3.1415927f / float(numMajor);
    const float minorStep = 6.2831855f / float(numMinor);
    for (int i = 0; i < numMajor; i++) {
        float a = float(i) * majorStep;
        float b = a + majorStep;
        float r0 = 1.0f * sms_msl_sinf(a);
        float r1 = 1.0f * sms_msl_sinf(b);
        float z0 = 1.0f * sms_msl_cosf(a);
        float z1 = 1.0f * sms_msl_cosf(b);
        begin(uint16_t((numMinor + 1) * 2));
        for (int j = 0; j <= numMinor; j++) {
            float c = float(j) * minorStep;
            float x = sms_msl_cosf(c);
            float y = sms_msl_sinf(c);
            vertex(x * r1, y * r1, z1);
            vertex(x * r0, y * r0, z0);
        }
    }
}

#endif  // SMS_GX_SDK_MATH_H
