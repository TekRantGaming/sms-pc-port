// Block compression for the mip levels a texture pack leaves out
// (gx_hires.cpp's completeChain): BC1-3, and BC7 in modes 6 and 5. Each 4x4
// block's endpoints are the extremes of its colours along their principal
// axis; that is plenty for minified levels, which are what is encoded here.
#include "gx_internal.h"

#include <math.h>
#include <string.h>
#include <algorithm>

namespace gx {

// The line along which a block's colours (the first `channels` of each pixel,
// those with use[i] set) spread most, as its two extreme points.
static void fitLine(const uint8_t* rgba, int channels, const bool* use, float lo[4], float hi[4]) {
    float mean[4] = {0, 0, 0, 0};
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (use && !use[i]) continue;
        for (int c = 0; c < channels; c++) mean[c] += rgba[i * 4 + c];
        n++;
    }
    if (!n) {
        memset(lo, 0, sizeof(float) * 4);
        memset(hi, 0, sizeof(float) * 4);
        return;
    }
    for (int c = 0; c < channels; c++) mean[c] /= float(n);
    float cov[4][4] = {}, mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (use && !use[i]) continue;
        float d[4];
        for (int c = 0; c < channels; c++) {
            d[c] = rgba[i * 4 + c] - mean[c];
            mn[c] = std::min(mn[c], float(rgba[i * 4 + c]));
            mx[c] = std::max(mx[c], float(rgba[i * 4 + c]));
        }
        for (int a = 0; a < channels; a++)
            for (int b = 0; b < channels; b++) cov[a][b] += d[a] * d[b];
    }
    // Power iteration from the bounding box's diagonal.
    float axis[4] = {0, 0, 0, 0}, len = 0;
    for (int c = 0; c < channels; c++) {
        axis[c] = mx[c] - mn[c];
        len += axis[c] * axis[c];
    }
    for (int it = 0; it < 8 && len > 1e-12f; it++) {
        float next[4] = {0, 0, 0, 0}, nl = 0;
        for (int a = 0; a < channels; a++) {
            for (int b = 0; b < channels; b++) next[a] += cov[a][b] * axis[b];
            nl += next[a] * next[a];
        }
        if (nl <= 1e-12f) break;
        nl = sqrtf(nl);
        for (int c = 0; c < channels; c++) axis[c] = next[c] / nl;
        len = 1;
    }
    float tmin = 0, tmax = 0;
    if (len > 1e-12f) {
        len = sqrtf(len);
        for (int c = 0; c < channels; c++) axis[c] /= len;
        tmin = 1e30f;
        tmax = -1e30f;
        for (int i = 0; i < 16; i++) {
            if (use && !use[i]) continue;
            float t = 0;
            for (int c = 0; c < channels; c++) t += (rgba[i * 4 + c] - mean[c]) * axis[c];
            tmin = std::min(tmin, t);
            tmax = std::max(tmax, t);
        }
    }
    for (int c = 0; c < 4; c++) {
        lo[c] = c < channels ? std::min(255.0f, std::max(0.0f, mean[c] + axis[c] * tmin)) : 0;
        hi[c] = c < channels ? std::min(255.0f, std::max(0.0f, mean[c] + axis[c] * tmax)) : 0;
    }
}

// A BC7 block's 128 bits, filled from the lowest.
struct Bits {
    uint64_t w[2] = {0, 0};
    int pos = 0;
    void put(uint32_t v, int n) {
        uint64_t x = uint64_t(v) & ((1ull << n) - 1);
        w[pos >> 6] |= x << (pos & 63);
        if ((pos & 63) + n > 64) w[1] |= x >> (64 - (pos & 63));
        pos += n;
    }
    void store(uint8_t* out) const {
        for (int b = 0; b < 16; b++) out[b] = uint8_t(w[b >> 3] >> (8 * (b & 7)));
    }
};

static const int kWeight2[4] = {0, 21, 43, 64};
static const int kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static int interp(int e0, int e1, int w) { return ((64 - w) * e0 + w * e1 + 32) >> 6; }

// BC7 mode 6: one subset, RGBA endpoints of 7 bits and a p-bit each, 4-bit
// indices. Returns the block's squared error.
static int encodeMode6(const uint8_t* rgba, uint8_t* out) {
    float ends[2][4];
    fitLine(rgba, 4, nullptr, ends[0], ends[1]);
    int q[2][4], p[2], e[2][4];
    for (int k = 0; k < 2; k++) {
        float best = 1e30f;
        for (int pb = 0; pb < 2; pb++) {
            int qq[4];
            float err = 0;
            for (int c = 0; c < 4; c++) {
                qq[c] = std::min(127, std::max(0, int(lrintf((ends[k][c] - pb) / 2))));
                float d = float(qq[c] << 1 | pb) - ends[k][c];
                err += d * d;
            }
            if (err < best) {
                best = err;
                p[k] = pb;
                memcpy(q[k], qq, sizeof qq);
            }
        }
        for (int c = 0; c < 4; c++) e[k][c] = q[k][c] << 1 | p[k];
    }
    int pal[16][4];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) pal[i][c] = interp(e[0][c], e[1][c], kWeight4[i]);
    int d[4], dd = 0;
    for (int c = 0; c < 4; c++) {
        d[c] = e[1][c] - e[0][c];
        dd += d[c] * d[c];
    }
    // The index whose weight is nearest each weight 0-64.
    static const struct Nearest {
        uint8_t of[65];
        Nearest() {
            for (int w = 0, j = 0; w <= 64; w++) {
                while (j < 15 && kWeight4[j + 1] - w < w - kWeight4[j]) j++;
                of[w] = uint8_t(j);
            }
        }
    } nearest;
    float scale = dd ? 64.0f / float(dd) : 0;
    int idx[16], total = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t* px = rgba + i * 4;
        // the projection's nearest weight, then its neighbours by error
        int dot = 0;
        for (int c = 0; c < 4; c++) dot += (px[c] - e[0][c]) * d[c];
        int guess = nearest.of[std::min(64, std::max(0, int(float(dot) * scale + 0.5f)))];
        int best = 0, bestErr = 1 << 30;
        for (int j = std::max(0, guess - 1); j <= std::min(15, guess + 1); j++) {
            int err = 0;
            for (int c = 0; c < 4; c++) err += (px[c] - pal[j][c]) * (px[c] - pal[j][c]);
            if (err < bestErr) {
                bestErr = err;
                best = j;
            }
        }
        idx[i] = best;
        total += bestErr;
    }
    if (idx[0] & 8) {  // the first index has no top bit: swap the endpoints
        std::swap(q[0], q[1]);
        std::swap(p[0], p[1]);
        for (int& i : idx) i = 15 - i;
    }
    Bits b;
    b.put(1 << 6, 7);
    for (int c = 0; c < 4; c++) {
        b.put(uint32_t(q[0][c]), 7);
        b.put(uint32_t(q[1][c]), 7);
    }
    b.put(uint32_t(p[0]), 1);
    b.put(uint32_t(p[1]), 1);
    b.put(uint32_t(idx[0]), 3);
    for (int i = 1; i < 16; i++) b.put(uint32_t(idx[i]), 4);
    b.store(out);
    return total;
}

// BC7 mode 5: colour endpoints of 7 bits and alpha endpoints of 8, each with
// its own 2-bit indices, for blocks whose alpha does not follow their colour
// (a glow's white texels around a dark core). Returns the squared error.
static int encodeMode5(const uint8_t* rgba, uint8_t* out) {
    float lo[4], hi[4];
    fitLine(rgba, 3, nullptr, lo, hi);
    int q[2][3], e[2][4];
    for (int c = 0; c < 3; c++) {
        q[0][c] = int(lrintf(lo[c] * 127 / 255));
        q[1][c] = int(lrintf(hi[c] * 127 / 255));
        e[0][c] = q[0][c] << 1 | q[0][c] >> 6;
        e[1][c] = q[1][c] << 1 | q[1][c] >> 6;
    }
    e[0][3] = 255;
    e[1][3] = 0;
    for (int i = 0; i < 16; i++) {
        e[0][3] = std::min(e[0][3], int(rgba[i * 4 + 3]));
        e[1][3] = std::max(e[1][3], int(rgba[i * 4 + 3]));
    }
    int cidx[16], aidx[16], total = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t* px = rgba + i * 4;
        int bestC = 1 << 30, bestA = 1 << 30;
        for (int j = 0; j < 4; j++) {
            int err = 0;
            for (int c = 0; c < 3; c++) {
                int v = interp(e[0][c], e[1][c], kWeight2[j]);
                err += (px[c] - v) * (px[c] - v);
            }
            if (err < bestC) {
                bestC = err;
                cidx[i] = j;
            }
            int a = px[3] - interp(e[0][3], e[1][3], kWeight2[j]);
            if (a * a < bestA) {
                bestA = a * a;
                aidx[i] = j;
            }
        }
        total += bestC + bestA;
    }
    if (cidx[0] & 2) {  // each set's first index has no top bit
        std::swap(q[0], q[1]);
        for (int& i : cidx) i = 3 - i;
    }
    if (aidx[0] & 2) {
        std::swap(e[0][3], e[1][3]);
        for (int& i : aidx) i = 3 - i;
    }
    Bits b;
    b.put(1 << 5, 6);
    b.put(0, 2);  // no channel rotation
    for (int c = 0; c < 3; c++) {
        b.put(uint32_t(q[0][c]), 7);
        b.put(uint32_t(q[1][c]), 7);
    }
    b.put(uint32_t(e[0][3]), 8);
    b.put(uint32_t(e[1][3]), 8);
    b.put(uint32_t(cidx[0]), 1);
    for (int i = 1; i < 16; i++) b.put(uint32_t(cidx[i]), 2);
    b.put(uint32_t(aidx[0]), 1);
    for (int i = 1; i < 16; i++) b.put(uint32_t(aidx[i]), 2);
    b.store(out);
    return total;
}

// BC7: mode 6, or mode 5 where that comes out closer (only where alpha
// varies: an opaque block's alpha cannot stray from its colour's line).
static void encodeBc7(const uint8_t* rgba, uint8_t* out) {
    int err = encodeMode6(rgba, out);
    bool flat = true;
    for (int i = 1; i < 16; i++) flat &= rgba[i * 4 + 3] == rgba[3];
    if (!err || flat) return;
    uint8_t other[16];
    if (encodeMode5(rgba, other) < err) memcpy(out, other, 16);
}

static uint16_t to565(const float* c) {
    return uint16_t(lrintf(c[0] * 31 / 255) << 11 | lrintf(c[1] * 63 / 255) << 5 | lrintf(c[2] * 31 / 255));
}

// A BC1 colour block (8 bytes). With `punchThrough`, pixels whose alpha is
// below 128 take the transparent index of BC1's three-colour mode; BC2 and
// BC3 always decode four colours.
static void encodeColour(const uint8_t* rgba, bool punchThrough, uint8_t* out) {
    bool use[16];
    for (int i = 0; i < 16; i++) use[i] = !punchThrough || rgba[i * 4 + 3] >= 128;
    float lo[4], hi[4];
    fitLine(rgba, 3, use, lo, hi);
    uint16_t c0 = to565(hi), c1 = to565(lo);
    if (punchThrough ? c0 > c1 : c0 < c1) std::swap(c0, c1);
    // The palette as the decoder (bcdec) builds it.
    int r0 = c0 >> 11, g0 = (c0 >> 5) & 63, b0 = c0 & 31, r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    int pal[4][3] = {{(r0 * 527 + 23) >> 6, (g0 * 259 + 33) >> 6, (b0 * 527 + 23) >> 6},
                     {(r1 * 527 + 23) >> 6, (g1 * 259 + 33) >> 6, (b1 * 527 + 23) >> 6}};
    int colours;
    if (c0 > c1 || !punchThrough) {
        int p2[3] = {((2 * r0 + r1) * 351 + 61) >> 7, ((2 * g0 + g1) * 2763 + 1039) >> 11, ((2 * b0 + b1) * 351 + 61) >> 7};
        int p3[3] = {((r0 + 2 * r1) * 351 + 61) >> 7, ((g0 + 2 * g1) * 2763 + 1039) >> 11, ((b0 + 2 * b1) * 351 + 61) >> 7};
        memcpy(pal[2], p2, sizeof p2);
        memcpy(pal[3], p3, sizeof p3);
        colours = c0 == c1 ? 1 : 4;
    } else {
        int p2[3] = {((r0 + r1) * 1053 + 125) >> 8, ((g0 + g1) * 4145 + 1019) >> 11, ((b0 + b1) * 1053 + 125) >> 8};
        memcpy(pal[2], p2, sizeof p2);
        colours = 3;
    }
    uint32_t indices = 0;
    for (int i = 0; i < 16; i++) {
        int best = 3;
        if (use[i]) {
            int bestErr = 1 << 30;
            for (int j = 0; j < colours; j++) {
                int err = 0;
                for (int c = 0; c < 3; c++) err += (rgba[i * 4 + c] - pal[j][c]) * (rgba[i * 4 + c] - pal[j][c]);
                if (err < bestErr) {
                    bestErr = err;
                    best = j;
                }
            }
        }
        indices |= uint32_t(best) << (2 * i);
    }
    out[0] = uint8_t(c0);
    out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1);
    out[3] = uint8_t(c1 >> 8);
    for (int b = 0; b < 4; b++) out[4 + b] = uint8_t(indices >> (8 * b));
}

// BC3's alpha block: the block's extremes and the six values between them.
static void encodeAlpha(const uint8_t* rgba, uint8_t* out) {
    int a0 = 0, a1 = 255;
    for (int i = 0; i < 16; i++) {
        a0 = std::max(a0, int(rgba[i * 4 + 3]));
        a1 = std::min(a1, int(rgba[i * 4 + 3]));
    }
    int pal[8] = {a0, a1};
    for (int j = 1; j < 7; j++) pal[j + 1] = ((7 - j) * a0 + j * a1) / 7;
    uint64_t bits = uint64_t(a0) | uint64_t(a1) << 8;
    for (int i = 0; i < 16 && a0 > a1; i++) {
        int best = 0;
        for (int j = 1; j < 8; j++)
            if (abs(rgba[i * 4 + 3] - pal[j]) < abs(rgba[i * 4 + 3] - pal[best])) best = j;
        bits |= uint64_t(best) << (16 + 3 * i);
    }
    for (int b = 0; b < 8; b++) out[b] = uint8_t(bits >> (8 * b));
}

void encodeBlock(int format, const uint8_t* rgba, uint8_t* out) {
    switch (format) {
    case BLOCK_BC1: {
        bool punchThrough = false;
        for (int i = 0; i < 16; i++) punchThrough |= rgba[i * 4 + 3] < 128;
        encodeColour(rgba, punchThrough, out);
        break;
    }
    case BLOCK_BC2: {
        uint64_t bits = 0;
        for (int i = 0; i < 16; i++) bits |= uint64_t((rgba[i * 4 + 3] * 15 + 127) / 255) << (4 * i);
        for (int b = 0; b < 8; b++) out[b] = uint8_t(bits >> (8 * b));
        encodeColour(rgba, false, out + 8);
        break;
    }
    case BLOCK_BC3:
        encodeAlpha(rgba, out);
        encodeColour(rgba, false, out + 8);
        break;
    default:
        encodeBc7(rgba, out);
        break;
    }
}

}  // namespace gx
