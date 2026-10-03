/* Host side of tools/gxmath/check.sh, built with platform/gx/src/gx_sdk_math.h
 * (the functions gx_api.cpp calls) and platform/misc/msl_math.c.
 *   harness gen N SEED   writes N random records for each of functions 0 to 2,
 *                        and one record per GXDrawSphere size in the list below
 *   harness run          reads records from stdin and writes each result
 * A record is 128 bytes, big-endian: u32 function, u32 integer argument, then
 * 30 floats. Functions and results (big-endian floats):
 *   0 GXInitLightDistAttn(f[0], f[1], iarg)       k[0..2], 0, 0, 0
 *   1 GXInitSpecularDir(f[0], f[1], f[2])         dir[0..2], pos[0..2]
 *   2 GXProject(f[0..2], mtx f[3..14], pm f[15..21], vp f[22..27])  sx, sy, sz, 0, 0, 0
 *   3 GXDrawSphere(iarg >> 8, iarg & 255)         each vertex's position and normal */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "gx_sdk_math.h"

static uint64_t s;
static uint64_t rnd() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double uni(double lo, double hi) { return lo + (hi - lo) * double(rnd() >> 11) * (1.0 / 9007199254740992.0); }
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

struct Rec {
    uint32_t fn, iarg;
    float f[30];
};

static void put(const Rec& r) {
    uint32_t w[32] = {be32(r.fn), be32(r.iarg)};
    for (int i = 0; i < 30; i++) w[2 + i] = be32(fb(r.f[i]));
    fwrite(w, 4, 32, stdout);
}

/* Any bit pattern but infinities and NaNs, below 2^60 in magnitude, so that a
 * sum of three squares stays finite (see gxsdkSpecularDir). */
static float bitsFinite() {
    for (;;) {
        float v = fbits(uint32_t(rnd()));
        if (fabsf(v) < 1.152921504606847e18f) return v;
    }
}

static void genDistAttn(Rec& r) {
    static const float kd[] = {0.0f, -0.0f, 1000.0f, 1.0f, -1.0f, 1e-30f, 3e38f};
    static const float kb[] = {0.0f, 1.0f, 0.5f, -0.0f, 0.99999994f, 1e-7f, 1.0000001f};
    r.iarg = uint32_t(rnd() % 5);  // GX_DA_OFF..GX_DA_STEEP, and an unknown function
    switch (rnd() % 5) {
    case 0: r.f[0] = float(uni(0, 10000)); break;
    case 1: r.f[0] = float(uni(-10, 10)); break;
    case 2: r.f[0] = kd[rnd() % 7]; break;
    case 3: r.f[0] = fbits(uint32_t(rnd())); break;
    default: r.f[0] = float(uni(0, 1) * ldexp(1.0, int(rnd() % 80) - 40)); break;
    }
    switch (rnd() % 5) {
    case 0: case 1: r.f[1] = float(uni(0, 1)); break;
    case 2: r.f[1] = kb[rnd() % 7]; break;
    case 3: r.f[1] = fbits(uint32_t(rnd())); break;
    default: r.f[1] = float(uni(-0.5, 1.5)); break;
    }
}

static void genSpecular(Rec& r) {
    switch (rnd() % 5) {
    case 0: case 1: {  // a unit vector, as the game's light directions
        double x = uni(-1, 1), y = uni(-1, 1), z = uni(-1, 1), m = sqrt(x * x + y * y + z * z);
        if (m == 0) m = 1;
        r.f[0] = float(x / m); r.f[1] = float(y / m); r.f[2] = float(z / m);
        break;
    }
    case 2:
        for (int i = 0; i < 3; i++) r.f[i] = float(uni(-2, 2));
        break;
    case 3:
        for (int i = 0; i < 3; i++) r.f[i] = float(uni(-10000, 10000));
        break;
    default:
        for (int i = 0; i < 3; i++) r.f[i] = (rnd() % 8) ? bitsFinite() : float(uni(-1, 1));
        break;
    }
    if (rnd() % 16 == 0) r.f[2] = 1.0f;  // n = (0, 0, 1) and close: vz near 0
}

static void genProject(Rec& r) {
    bool wild = rnd() % 16 == 0;  // every float any bit pattern
    for (int i = 0; i < 3; i++) r.f[i] = float(uni(-20000, 20000));
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.f[3 + i * 4 + j] = float(uni(-2, 2));
        r.f[3 + i * 4 + 3] = float(uni(-30000, 30000));
    }
    float* pm = r.f + 15;
    if (rnd() & 1) {  // GXSetProjection's perspective values
        pm[0] = 0.0f;
        pm[1] = float(uni(0.3, 3)); pm[2] = float(uni(-0.2, 0.2));
        pm[3] = float(uni(0.3, 3)); pm[4] = float(uni(-0.2, 0.2));
        pm[5] = float(uni(-1.001, -0.0001)); pm[6] = float(uni(-200, -0.1));
    } else {  // orthographic
        pm[0] = 1.0f;
        for (int i = 1; i < 7; i++) pm[i] = float(uni(-2, 2));
    }
    float* vp = r.f + 22;
    if (rnd() & 1) {  // the game's viewports
        vp[0] = 0.0f; vp[1] = (rnd() & 1) ? 0.0f : -0.5f; vp[2] = 640.0f; vp[3] = 448.0f; vp[4] = 0.0f; vp[5] = 1.0f;
    } else {
        vp[0] = float(uni(0, 200)); vp[1] = float(uni(-1, 200)); vp[2] = float(uni(1, 1000));
        vp[3] = float(uni(1, 1000)); vp[4] = float(uni(0, 0.5)); vp[5] = float(uni(0.5, 1));
    }
    if (wild)
        for (int i = 0; i < 28; i++) r.f[i] = fbits(uint32_t(rnd()));
}

static std::vector<float> stream;
static void sBegin(uint16_t) {}
static void sVertex(float x, float y, float z) {
    float v[6] = {x, y, z, x, y, z};  // gx_api.cpp's sphereVertex: the normal is the position
    stream.insert(stream.end(), v, v + 6);
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "gen")) {
        long n = atol(argv[2]);
        s = strtoull(argv[3], 0, 0) | 1;
        for (uint32_t fn = 0; fn < 3; fn++)
            for (long i = 0; i < n; i++) {
                Rec r;
                memset(&r, 0, sizeof r);
                r.fn = fn;
                if (fn == 0) genDistAttn(r);
                else if (fn == 1) genSpecular(r);
                else genProject(r);
                put(r);
            }
        /* GXDrawSphere: every size up to 16 x 32 (the game draws 8 x 16), and a few large ones */
        static const int big[][2] = {{255, 255}, {64, 128}, {100, 3}, {3, 200}, {33, 65}};
        for (int ma = 1; ma <= 16; ma++)
            for (int mi = 1; mi <= 32; mi++) {
                Rec r;
                memset(&r, 0, sizeof r);
                r.fn = 3; r.iarg = uint32_t(ma << 8 | mi);
                put(r);
            }
        for (auto& b : big) {
            Rec r;
            memset(&r, 0, sizeof r);
            r.fn = 3; r.iarg = uint32_t(b[0] << 8 | b[1]);
            put(r);
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "run")) {
        uint32_t w[32];
        while (fread(w, 4, 32, stdin) == 32) {
            Rec r;
            r.fn = be32(w[0]); r.iarg = be32(w[1]);
            for (int i = 0; i < 30; i++) r.f[i] = fbits(be32(w[2 + i]));
            float o[6] = {0, 0, 0, 0, 0, 0};
            switch (r.fn) {
            case 0: gxsdkLightDistAttn(r.f[0], r.f[1], int(r.iarg), o); break;
            case 1: gxsdkSpecularDir(r.f[0], r.f[1], r.f[2], o, o + 3); break;
            case 2: {
                float mtx[3][4];
                memcpy(mtx, r.f + 3, sizeof mtx);
                gxsdkProject(r.f[0], r.f[1], r.f[2], mtx, r.f + 15, r.f + 22, o, o + 1, o + 2);
                break;
            }
            default:
                stream.clear();
                gxsdkSphere(uint8_t(r.iarg >> 8), uint8_t(r.iarg), sBegin, sVertex);
                for (float v : stream) {
                    uint32_t b = be32(fb(v));
                    fwrite(&b, 4, 1, stdout);
                }
                continue;
            }
            for (int i = 0; i < 6; i++) {
                uint32_t b = be32(fb(o[i]));
                fwrite(&b, 4, 1, stdout);
            }
        }
        return 0;
    }
    fprintf(stderr, "usage: harness gen N SEED | run\n");
    return 2;
}
