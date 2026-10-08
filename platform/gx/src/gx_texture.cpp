// Texture decoding (GameCube tiled formats -> RGBA8) and the GL texture cache.
#include "gx_internal.h"
#include "gl_funcs.h"
#include "gx_glcache.h"

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <unordered_map>

namespace gx {

extern uint32_t g_statTexUploads;
extern uint64_t g_statTexHashBytes, g_statTexInvalidates;

static inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

static inline void rgb565(uint16_t v, uint8_t* o) {
    o[0] = uint8_t((v >> 11) << 3 | (v >> 13));
    o[1] = uint8_t(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
    o[2] = uint8_t((v & 31) << 3 | ((v >> 2) & 7));
    o[3] = 255;
}
static inline void rgb5a3(uint16_t v, uint8_t* o) {
    if (v & 0x8000) {
        o[0] = uint8_t(((v >> 10) & 31) << 3 | ((v >> 12) & 7));
        o[1] = uint8_t(((v >> 5) & 31) << 3 | ((v >> 7) & 7));
        o[2] = uint8_t((v & 31) << 3 | ((v >> 2) & 7));
        o[3] = 255;
    } else {
        o[0] = uint8_t(((v >> 8) & 15) * 17);
        o[1] = uint8_t(((v >> 4) & 15) * 17);
        o[2] = uint8_t((v & 15) * 17);
        o[3] = uint8_t(((v >> 12) & 7) * 255 / 7);
    }
}
static inline void ia8(uint16_t v, uint8_t* o) {
    o[0] = o[1] = o[2] = uint8_t(v & 0xFF);
    o[3] = uint8_t(v >> 8);
}

static const uint8_t kTileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileBytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};

uint32_t texLevelBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    fmt &= 15;
    if (!kTileW[fmt]) return 0;
    uint32_t cols = (w + kTileW[fmt] - 1) / kTileW[fmt];
    uint32_t rows = (h + kTileH[fmt] - 1) / kTileH[fmt];
    return cols * rows * kTileBytes[fmt];
}

static void palette(const uint8_t* tlut, uint32_t tlutFmt, uint32_t idx, uint8_t* o) {
    uint16_t v = be16(tlut + 2 * idx);
    if (tlutFmt == 0) ia8(v, o);
    else if (tlutFmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

// Decodes one mip level into out (w*h*4 bytes, row-major, top row first).
void decodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutFmt,
                   uint8_t* out) {
    fmt &= 15;
    uint32_t tw = kTileW[fmt], th = kTileH[fmt];
    if (!tw) {
        memset(out, 0xFF, size_t(w) * h * 4);
        return;
    }
    uint32_t cols = (w + tw - 1) / tw, rows = (h + th - 1) / th;
    const uint8_t* p = src;
    for (uint32_t ty = 0; ty < rows; ty++) {
        for (uint32_t tx = 0; tx < cols; tx++) {
            uint32_t x0 = tx * tw, y0 = ty * th;
            auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
                if (x < w && y < h) memcpy(out + (size_t(y) * w + x) * 4, c, 4);
            };
            uint8_t c[4];
            switch (fmt) {
            case 0:  // I4
                for (uint32_t i = 0; i < 64; i++) {
                    uint8_t b = p[i >> 1];
                    uint8_t v = (i & 1) ? (b & 15) : (b >> 4);
                    c[0] = c[1] = c[2] = c[3] = uint8_t(v * 17);
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 1:  // I8
                for (uint32_t i = 0; i < 32; i++) {
                    c[0] = c[1] = c[2] = c[3] = p[i];
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 2:  // IA4
                for (uint32_t i = 0; i < 32; i++) {
                    c[0] = c[1] = c[2] = uint8_t((p[i] & 15) * 17);
                    c[3] = uint8_t((p[i] >> 4) * 17);
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 3:  // IA8
                for (uint32_t i = 0; i < 16; i++) {
                    ia8(be16(p + 2 * i), c);
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            case 4:  // RGB565
                for (uint32_t i = 0; i < 16; i++) {
                    rgb565(be16(p + 2 * i), c);
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            case 5:  // RGB5A3
                for (uint32_t i = 0; i < 16; i++) {
                    rgb5a3(be16(p + 2 * i), c);
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            case 6:  // RGBA8: AR plane then GB plane
                for (uint32_t i = 0; i < 16; i++) {
                    c[3] = p[2 * i];
                    c[0] = p[2 * i + 1];
                    c[1] = p[32 + 2 * i];
                    c[2] = p[32 + 2 * i + 1];
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            case 8:  // C4
                for (uint32_t i = 0; i < 64; i++) {
                    uint8_t b = p[i >> 1];
                    palette(tlut, tlutFmt, (i & 1) ? (b & 15) : (b >> 4), c);
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 9:  // C8
                for (uint32_t i = 0; i < 32; i++) {
                    palette(tlut, tlutFmt, p[i], c);
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 10:  // C14X2
                for (uint32_t i = 0; i < 16; i++) {
                    palette(tlut, tlutFmt, be16(p + 2 * i) & 0x3FFF, c);
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            case 14:  // CMPR: 2x2 DXT1 sub-blocks
                for (uint32_t sb = 0; sb < 4; sb++) {
                    const uint8_t* b = p + sb * 8;
                    uint16_t c0 = be16(b), c1 = be16(b + 2);
                    uint8_t pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    uint32_t sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    for (uint32_t r = 0; r < 4; r++) {
                        uint8_t bits = b[4 + r];
                        for (uint32_t q = 0; q < 4; q++) put(sx + q, sy + r, pal[(bits >> (6 - 2 * q)) & 3]);
                    }
                }
                break;
            default:
                break;
            }
            p += kTileBytes[fmt];
        }
    }
}

// ------------------------------------------------------------------ cache
struct TexKey {
    const void* ptr;
    uint32_t fmt, w, h, levels, tlutOff, tlutFmt;
    bool operator==(const TexKey& o) const {
        return ptr == o.ptr && fmt == o.fmt && w == o.w && h == o.h && levels == o.levels && tlutOff == o.tlutOff &&
               tlutFmt == o.tlutFmt;
    }
};
struct TexKeyHash {
    size_t operator()(const TexKey& k) const { return size_t(hashBytes(&k, sizeof(k))); }
};
struct TexEntry {
    GLuint tex = 0;
    uint64_t dataHash = 0, tlutHash = 0;
    uint32_t checkedGen = 0;
    uint32_t bytes = 0;
    std::string hires;  // the texture pack's replacement for the current data, if any
    int prompt = -1;    // the button prompt glyph this picture shows (promptGlyph), or -1
};

static std::unordered_map<TexKey, TexEntry, TexKeyHash> s_cache;
// The cache's entries by address (sorted lazily), so a cache flush finds the
// textures it overlaps without walking the whole cache. Entries are never
// erased before shutdown, so the pointers stay valid.
static std::vector<std::pair<uintptr_t, TexEntry*>> s_ranges;
static bool s_rangesSorted = true;
static uint32_t s_maxBytes = 0;
static uint32_t s_gen = 1;
static std::vector<uint8_t> s_decodeBuf;

struct CopyEntry {
    GLuint tex;
    int w, h;
    uint32_t fmt;
    uint32_t bytes;  // size of the destination in RAM (0 if not written back)
};
static std::unordered_map<const void*, CopyEntry> s_copies;

static void drainDeletedCopies();

void textureInvalidateAll() {
    s_gen++;
    g_statTexInvalidates++;
}

void textureInvalidateRange(const void* p, uint32_t size) {
    if (!s_rangesSorted) {
        std::sort(s_ranges.begin(), s_ranges.end(),
                  [](const std::pair<uintptr_t, TexEntry*>& a, const std::pair<uintptr_t, TexEntry*>& b) {
                      return a.first < b.first;
                  });
        s_rangesSorted = true;
    }
    uintptr_t lo = reinterpret_cast<uintptr_t>(p), hi = lo + size;
    uintptr_t from = lo > s_maxBytes ? lo - s_maxBytes : 0;
    auto it = std::lower_bound(s_ranges.begin(), s_ranges.end(), from,
                               [](const std::pair<uintptr_t, TexEntry*>& a, uintptr_t v) { return a.first < v; });
    for (; it != s_ranges.end() && it->first < hi; ++it)
        if (it->first + it->second->bytes > lo) it->second->checkedGen = 0;
}

void textureShutdown() {
    glcInvalidate();
    hiresShutdown();
    for (auto& kv : s_cache) glDeleteTextures(1, &kv.second.tex);
    s_cache.clear();
    s_ranges.clear();
    s_rangesSorted = true;
    s_maxBytes = 0;
    for (auto& kv : s_copies) glDeleteTextures(1, &kv.second.tex);
    s_copies.clear();
    drainDeletedCopies();
}

unsigned efbCopyLookup(const void* addr, int* w, int* h) {
    auto it = s_copies.find(addr);
    if (it == s_copies.end()) return 0;
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.tex;
}

void efbCopyRegister(const void* addr, unsigned tex, int w, int h, uint32_t fmt) {
    drainDeletedCopies();
    auto it = s_copies.find(addr);
    if (it != s_copies.end() && it->second.tex != tex) {
        glcForgetTexture(it->second.tex);
        glDeleteTextures(1, &it->second.tex);
    }
    s_copies[addr] = CopyEntry{tex, w, h, fmt, 0};
}

void efbCopySetBytes(const void* addr, uint32_t bytes) {
    auto it = s_copies.find(addr);
    if (it != s_copies.end()) it->second.bytes = bytes;
}

// The CPU wrote [p, p + size) (DCFlushRange/DCStoreRange): RAM is the truth
// again, so cached decodes are re-checked and EFB copies that were written
// back into that memory stop shadowing it.
// Called from any game thread (loaders flush their buffers too), and only the
// thread that owns the GL context may call GL: dropped copies' textures are
// queued and deleted by that thread later (drainDeletedCopies).
static std::vector<GLuint> s_deadCopyTex;

static void drainDeletedCopies() {
    if (s_deadCopyTex.empty()) return;
    for (GLuint t : s_deadCopyTex) glcForgetTexture(t);
    glDeleteTextures(GLsizei(s_deadCopyTex.size()), s_deadCopyTex.data());
    s_deadCopyTex.clear();
}

void textureCpuWrote(const void* p, uint32_t size) {
    textureInvalidateRange(p, size);
    const uint8_t* lo = static_cast<const uint8_t*>(p);
    const uint8_t* hi = lo + size;
    for (auto it = s_copies.begin(); it != s_copies.end();) {
        const uint8_t* a = static_cast<const uint8_t*>(it->first);
        if (it->second.bytes && a < hi && a + it->second.bytes > lo) {
            s_deadCopyTex.push_back(it->second.tex);
            it = s_copies.erase(it);
        } else {
            ++it;
        }
    }
}

// Memory layout (texture format) an EFB copy of copy-format nibble `f`
// produces, as GXCopyTex would store it.
uint32_t copyLayout(uint32_t f, bool z) {
    switch (f) {
    case 0: return 0;                               // I4 / R4 / Z4
    case 2: return 2;                               // IA4 / RA4
    case 3: case 11: case 12: return 3;             // IA8 / RA8 / RG8 / GB8 / Z16*
    case 4: return z ? 3 : 4;                       // RGB565
    case 5: return 5;                               // RGB5A3
    case 6: return 6;                               // RGBA8 / Z24X8
    default: return 1;                              // I8, A8, R8, G8, B8, Z8*
    }
}

// Inverse of decodeTexture for the formats EFB copies produce: `rgba` is
// w*h texels, top row first, in the form decodeTexture returns.
uint32_t encodeTexture(const uint8_t* rgba, uint32_t fmt, uint32_t w, uint32_t h, uint8_t* dst) {
    uint32_t tw = kTileW[fmt], th = kTileH[fmt];
    if (!tw) return 0;
    uint32_t cols = (w + tw - 1) / tw, rows = (h + th - 1) / th;
    uint8_t* p = dst;
    static const uint8_t kZero[4] = {0, 0, 0, 0};
    for (uint32_t ty = 0; ty < rows; ty++) {
        for (uint32_t tx = 0; tx < cols; tx++) {
            auto at = [&](uint32_t i, uint32_t tileW) -> const uint8_t* {
                uint32_t x = tx * tw + i % tileW, y = ty * th + i / tileW;
                return (x < w && y < h) ? rgba + (size_t(y) * w + x) * 4 : kZero;
            };
            switch (fmt) {
            case 0:  // I4
                for (uint32_t i = 0; i < 64; i += 2)
                    p[i >> 1] = uint8_t((at(i, 8)[0] * 15 + 127) / 255 << 4 | (at(i + 1, 8)[0] * 15 + 127) / 255);
                break;
            case 1:  // I8
                for (uint32_t i = 0; i < 32; i++) p[i] = at(i, 8)[0];
                break;
            case 2:  // IA4
                for (uint32_t i = 0; i < 32; i++) {
                    const uint8_t* c = at(i, 8);
                    p[i] = uint8_t((c[3] * 15 + 127) / 255 << 4 | (c[0] * 15 + 127) / 255);
                }
                break;
            case 3:  // IA8
                for (uint32_t i = 0; i < 16; i++) {
                    const uint8_t* c = at(i, 4);
                    p[2 * i] = c[3];
                    p[2 * i + 1] = c[0];
                }
                break;
            case 4:  // RGB565
                for (uint32_t i = 0; i < 16; i++) {
                    const uint8_t* c = at(i, 4);
                    uint16_t v = uint16_t((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3));
                    p[2 * i] = uint8_t(v >> 8);
                    p[2 * i + 1] = uint8_t(v);
                }
                break;
            case 5:  // RGB5A3
                for (uint32_t i = 0; i < 16; i++) {
                    const uint8_t* c = at(i, 4);
                    uint16_t v;
                    if (c[3] >= 0xE0)
                        v = uint16_t(0x8000 | (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | (c[2] >> 3));
                    else
                        v = uint16_t((c[3] >> 5) << 12 | (c[0] >> 4) << 8 | (c[1] >> 4) << 4 | (c[2] >> 4));
                    p[2 * i] = uint8_t(v >> 8);
                    p[2 * i + 1] = uint8_t(v);
                }
                break;
            case 6:  // RGBA8: AR plane then GB plane
                for (uint32_t i = 0; i < 16; i++) {
                    const uint8_t* c = at(i, 4);
                    p[2 * i] = c[3];
                    p[2 * i + 1] = c[0];
                    p[32 + 2 * i] = c[1];
                    p[32 + 2 * i + 1] = c[2];
                }
                break;
            }
            p += kTileBytes[fmt];
        }
    }
    return uint32_t(p - dst);
}

// GX sampler state (wrap, filters, LOD bias and clamp) as a GL sampler
// object, one per distinct setting, so binding a texture costs at most a
// sampler bind instead of seven glTexParameter calls.
static std::unordered_map<uint64_t, GLuint> s_samplers;

// SMS_ANISO=n: anisotropic filtering for every filtered texture, clamped to
// the driver's maximum (EXT/ARB_texture_filter_anisotropic, core in GL 4.6).
static float anisotropy() {
    static float s_aniso = -1.0f;
    if (s_aniso >= 0.0f) return s_aniso;
    s_aniso = 0.0f;
    const char* e = getenv("SMS_ANISO");
    const float want = e ? float(atof(e)) : 0.0f;
    if (want <= 1.0f) return s_aniso;
    while (glGetError() != GL_NO_ERROR) {}
    GLint maxAniso = 0;
    glGetIntegerv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &maxAniso);
    if (glGetError() == GL_NO_ERROR && maxAniso > 1) s_aniso = std::min(want, float(maxAniso));
    if (s_aniso > 1.0f) logmsg("anisotropic filtering %gx", double(s_aniso));
    else logmsg("anisotropic filtering is not supported by this driver");
    return s_aniso;
}

// hires: log2 of a texture pack replacement's size over the GX size (-1 for
// none). Its LOD range moves up by that much, so the same screen size samples
// the same level of detail; a replacement of a texture without mipmaps gets
// generated ones down to the original size, so it does not shimmer.
static GLuint samplerFor(uint32_t mode0, uint32_t mode1, uint32_t levels, int hires = -1) {
    uint64_t key = uint64_t(mode0 & 0x1FFFF) | uint64_t(mode1 & 0xFFFF) << 17 | uint64_t(levels > 1) << 33 |
                   uint64_t(hires + 1) << 34;
    GLuint& smp = s_samplers[key];
    if (smp) return smp;
    glGenSamplers(1, &smp);
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    uint32_t mf = (mode0 >> 5) & 7;
    bool lin = (mf & 4) != 0;
    uint32_t mip = mf & 3;  // 0 none, 1 nearest mip, 2 linear mip
    GLint minf;
    float minLod = float(mode1 & 0xFF) / 16.0f, maxLod = float((mode1 >> 8) & 0xFF) / 16.0f;
    if (hires > 0 && (levels <= 1 || mip == 0)) {  // a plain texture's upscale
        minf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        minLod = 0;
        maxLod = float(hires);
    } else if (levels <= 1 || mip == 0) {
        minf = lin ? GL_LINEAR : GL_NEAREST;
    } else {
        minf = mip == 1 ? (lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                        : (lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
        if (hires > 0) {
            minLod += float(hires);
            maxLod += float(hires);
        }
    }
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, minf);
    glSamplerParameterf(smp, GL_TEXTURE_LOD_BIAS, float(int8_t((mode0 >> 9) & 0xFF)) / 32.0f);
    glSamplerParameterf(smp, GL_TEXTURE_MIN_LOD, minLod);
    glSamplerParameterf(smp, GL_TEXTURE_MAX_LOD, maxLod);
    if (minf != GL_NEAREST && minf != GL_NEAREST_MIPMAP_NEAREST && anisotropy() > 1.0f)
        glSamplerParameterf(smp, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, anisotropy());
    return smp;
}

static GLuint s_whiteTex = 0;

// The HUD's pictures of a GameCube button, by their data (the hashes texture
// packs name them by), and the button prompt glyph each shows; -1 for others.
static int promptGlyph(const uint8_t* ptr, uint32_t fmt, uint32_t w, uint32_t h) {
    if (fmt == 2 && w == 34 && h == 44 && xxh64(ptr, texLevelBytes(fmt, w, h), 0) == 0xc9717b966554213full)
        return 2;  // the FLUDD gauge's X (water_button_x)
    return -1;
}

// The button prompt in place of such a picture: the glyph's 64x64 image centred
// on a canvas twice the picture's size, one GL texture per style and glyph.
// 0 while the game's own pictures are shown.
static GLuint promptTexture(int glyph, int map, uint32_t w, uint32_t h) {
    int style = 0;
    const uint8_t* img = promptImage(glyph, &style);
    if (!img) return 0;
    static std::unordered_map<int, GLuint> s_prompts;
    GLuint& t = s_prompts[style * 8 + glyph];
    if (t) return t;
    const int cw = int(w) * 2, ch = int(h) * 2, ox = (cw - 64) / 2, oy = (ch - 64) / 2;
    std::vector<uint8_t> canvas(size_t(cw) * ch * 4, 0);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int cx = ox + x, cy = oy + y;
            if (cx >= 0 && cy >= 0 && cx < cw && cy < ch) memcpy(&canvas[(size_t(cy) * cw + cx) * 4], img + (y * 64 + x) * 4, 4);
        }
    glGenTextures(1, &t);
    glcActiveUnit(map);
    glBindTexture(GL_TEXTURE_2D, t);
    glcNoteBound(map, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, canvas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

unsigned bindTextureMap(int map, float* outW, float* outH) {
    drainDeletedCopies();
    uint32_t mode0 = g.bp[bpTexReg(BP_TX_MODE0, map)];
    uint32_t mode1 = g.bp[bpTexReg(BP_TX_MODE1, map)];
    uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
    uint32_t tlutReg = g.bp[bpTexReg(BP_TX_TLUT, map)];
    uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    const uint8_t* ptr = g.texImage[map];
    *outW = float(w);
    *outH = float(h);

    int cw, ch;
    if (GLuint ct = efbCopyLookup(ptr, &cw, &ch)) {
        glcBindTexture(map, ct);
        glcBindSampler(map, samplerFor(mode0, 0, 1));
        return ct;
    }
    if (!ptr) {
        if (!s_whiteTex) {
            glGenTextures(1, &s_whiteTex);
            glcActiveUnit(map);
            glBindTexture(GL_TEXTURE_2D, s_whiteTex);
            glcNoteBound(map, s_whiteTex);
            uint32_t px = 0xFFFFFFFFu;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
        }
        glcBindTexture(map, s_whiteTex);
        glcBindSampler(map, 0);  // its own (default) parameters, as before samplers
        return s_whiteTex;
    }

    // mip levels present: GX has no explicit count, derive it from max LOD
    uint32_t levels = 1;
    if (((mode0 >> 5) & 3) != 0) {
        uint32_t maxLod = ((mode1 >> 8) & 0xFF) / 16;
        uint32_t m = w > h ? w : h;
        while (levels <= maxLod && (m >> levels) >= 1) levels++;
        if (levels > 11) levels = 11;
    }
    bool ci = fmt == 8 || fmt == 9 || fmt == 10;
    TexKey key{ptr, fmt, w, h, levels, ci ? (tlutReg & 0x3FF) : 0, ci ? ((tlutReg >> 10) & 3) : 0};
    const uint8_t* tlut = ci ? g.tlutMem + ((tlutReg & 0x3FF) << 9) : nullptr;
    uint32_t tlutBytes = fmt == 8 ? 32 : fmt == 9 ? 512 : 0x8000;
    if (tlut && ((tlutReg & 0x3FF) << 9) + tlutBytes > TMEM_TLUT_SIZE) tlutBytes = TMEM_TLUT_SIZE - ((tlutReg & 0x3FF) << 9);

    TexEntry& e = s_cache[key];
    bool upload = false;
    uint32_t total = 0;
    {
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < levels; l++) {
            total += texLevelBytes(fmt, lw, lh);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
    }
    if (!e.tex) {
        glGenTextures(1, &e.tex);
        upload = true;
        s_ranges.emplace_back(reinterpret_cast<uintptr_t>(ptr), &e);
        s_rangesSorted = false;
        if (total > s_maxBytes) s_maxBytes = total;
    }
    if (e.checkedGen != s_gen || upload) {
        g_statTexHashBytes += total;
        uint64_t dh = hashBytes(ptr, total);
        uint64_t th = tlut ? hashBytes(tlut, tlutBytes) : 0;
        if (upload || dh != e.dataHash || th != e.tlutHash) upload = true;
        e.dataHash = dh;
        e.tlutHash = th;
        e.checkedGen = s_gen;
        e.bytes = total;
    }
    glcBindTexture(map, e.tex);
    // the pack names' _m: mipmap filtering with a maximum LOD above 0
    bool mipmapped = ((mode0 >> 5) & 3) != 0 && ((mode1 >> 8) & 0xFF) != 0;
    if (upload && hiresDumpDir()) {  // the pack name, and the image, for making a pack
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        decodeTexture(ptr, fmt, w, h, tlut, (tlutReg >> 10) & 3, rgba.data());
        hiresDump(hiresName(ptr, fmt, w, h, mipmapped, tlut, tlutBytes, false), rgba.data(), w, h);
    }
    if (upload && hiresEnabled()) {  // a texture pack's replacement for the new data
        e.hires = hiresName(ptr, fmt, w, h, mipmapped, tlut, tlutBytes, true);
        // the same image dumped from a use with the other mipmap setting
        if (e.hires.empty()) e.hires = hiresName(ptr, fmt, w, h, !mipmapped, tlut, tlutBytes, true);
        if (hiresLog()) {
            std::string n = e.hires.empty() ? hiresName(ptr, fmt, w, h, mipmapped, tlut, tlutBytes, false) : e.hires;
            logmsg("texture %p %s%s", static_cast<const void*>(ptr), n.c_str(), e.hires.empty() ? "" : " (replaced)");
        }
    }
    if (upload) {
        glcActiveUnit(map);  // the upload targets the active unit's texture
        g_statTexUploads++;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const uint8_t* src = ptr;
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < levels; l++) {
            s_decodeBuf.resize(size_t(lw) * lh * 4);
            decodeTexture(src, fmt, lw, lh, tlut, (tlutReg >> 10) & 3, s_decodeBuf.data());
            glTexImage2D(GL_TEXTURE_2D, GLint(l), GL_RGBA8, GLsizei(lw), GLsizei(lh), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         s_decodeBuf.data());
            src += texLevelBytes(fmt, lw, lh);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
    }
    if (upload) e.prompt = promptGlyph(ptr, fmt, w, h);
    if (e.prompt >= 0) {  // a button picture, drawn as the button prompt in use
        if (GLuint t = promptTexture(e.prompt, map, w, h)) {
            glcBindTexture(map, t);
            glcBindSampler(map, samplerFor(mode0, mode1, 1, 1));
            return t;
        }
    }
    if (!e.hires.empty()) {  // the replacement, once decoded (the original meanwhile)
        int scale = 0;
        if (GLuint t = hiresTexture(e.hires, map, w, h, &scale)) {
            glcBindTexture(map, t);
            glcBindSampler(map, samplerFor(mode0, mode1, levels, scale));
            return t;
        }
    }
    glcBindSampler(map, samplerFor(mode0, mode1, levels));
    return e.tex;
}

}  // namespace gx
