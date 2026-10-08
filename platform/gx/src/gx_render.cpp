// OpenGL 3.3 backend: EFB as an FBO, batched primitive submission, GL state
// derived from the BP/XF registers, EFB copies (display and texture) and XFB
// presentation.
#include "gx_internal.h"
#include "gl_funcs.h"
#include "gx_glcache.h"
#include "sms_gx/gx_pc.h"

#include <math.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unordered_map>

namespace gx {

uint32_t g_statTexUploads, g_statShaderCompiles;
uint64_t g_statTexHashBytes, g_statTexInvalidates;
namespace gl {
extern uint64_t g_statGlCalls;
void logTopCalls(uint32_t frames);
}
extern double g_decodeSeconds;
static GXPCStats s_stats;
void shaderShutdown();
void invalidateOverlay();

// SMS_GX_STATS=n: every n display frames, log draws, uploads and the wall time
// spent inside sms_gx (flushes, texture decode, copies) against the frame time.
static double s_gxSeconds = 0, s_texSeconds = 0, s_copySeconds = 0, s_peekSeconds = 0;
double g_flushSeconds = 0;  // in flushBatch (read by the vertex loader timer)
static double s_waitSeconds = 0;  // blocked on the GPU
double g_presentSeconds = 0, g_swapSeconds = 0;  // GXPC_Present (gx_platform.cpp)
static double (*s_idleClock)(void) = nullptr;
static double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}
// The breakdown timers (textures, draws, copies, peeks, GPU waits, the vertex
// loader) run with SMS_GX_STATS or while the overlay is open
// (GXPC_SetDetailedTimers). Otherwise only the overall sms_gx time is
// measured; the others cost a clock read per texture bind and per primitive.
static bool statsEnv() {
    const char* e = getenv("SMS_GX_STATS");
    return e && atoi(e) > 0;
}
bool g_gxStats = statsEnv();
struct GxTimer {
    double* acc;
    double t0;
    explicit GxTimer(double* a = &s_gxSeconds) : acc(a == &s_gxSeconds || g_gxStats ? a : nullptr),
                                                 t0(acc ? nowSeconds() : 0) {}
    ~GxTimer() {
        if (acc) *acc += nowSeconds() - t0;
    }
};

enum { EFB_W = 640, EFB_H = 528 };

static int s_scale = 1;

// Widescreen (GXPC_SetWidescreen): the EFB is s_efbW = 640 * s_wide wide
// while the game keeps working in 640-wide coordinates, which each draw,
// copy and peek maps into it (XMap: x' = a * x + b, clip x scaled by clip):
//   stretched (a = s_wide): the game camera, which the widescreen patch
//     widens itself, 2D that spans the whole width (fades, screen copies
//     drawn back), full-width copies and clears, peeks;
//   centred (b = s_ox): 2D and 3D that cover part of the screen (the HUD).
// A full-screen perspective draw whose projection the game did not widen
// (title, file select, cutscene overlays) is stretched with its clip x
// scaled by 1 / s_wide: its field of view widens, and what the game placed
// at x lands on x + s_ox, beside the centred HUD.
static float s_wide = 1.0f;
static int s_efbW = EFB_W, s_ox = 0;
struct XMap {
    float a = 1, b = 0, clip = 1;
};
static XMap s_xmap;
static bool s_stretch2D = false;  // GXPC_SetStretch2D: the game's faders
// SMS_WIDESCREEN_HUD=edges: while the game draws its gameplay HUD
// (GXPC_SetHud), a piece in the left third of the 4:3 frame goes to the left
// edge of the wide one and one in the right third to the right edge; the
// middle stays centred. A piece is the outermost J2D pane being drawn that
// is narrower than three quarters of the screen (wider ones are the HUD's
// screen-sized containers), with everything in it (GXPC_HudPaneBegin), or
// else a draw on its own.
static bool s_hudEdges = false, s_hud = false;
struct HudPane {
    float x1, x2;
};
static std::vector<HudPane> s_hudPanes;  // the J2D panes being drawn, outermost first
static float s_hudAnchor = -1.0f;        // the piece's centre (game x), -1 for none

static float hudOffset(float at) {
    if (at < float(EFB_W) / 3) return 0.0f;
    if (at > float(EFB_W) * 2 / 3) return float(2 * s_ox);
    return float(s_ox);
}
// the offset of a centred draw whose own centre is at game x `at`
static float centredOffset(float at) {
    if (!s_hud) return float(s_ox);
    return hudOffset(s_hudAnchor >= 0.0f ? s_hudAnchor : at);
}
// the game camera's aspect (TMarDirector: video width 660 * 0.91346 / 448)
static const float kCamAspect = 660.0f * 0.91346145f / 448.0f;
static GLuint s_efbFbo, s_efbColor, s_efbDepth;
// SMS_MSAA=n: the game draws into s_efbFbo, built from multisampled
// renderbuffers; every read of the EFB (copies, peeks) first resolves it into
// s_efbResolveFbo, which holds s_efbColor and s_efbDepth.
static int s_msaa = 0;
static GLuint s_efbResolveFbo, s_efbMsColor, s_efbMsDepth;
static GLuint s_vao;
static GLuint s_copyProg, s_copyVao;
static GLint s_copyUMode, s_copyURect, s_copyUAlphaOne;
static GLuint s_tmpFbo;
static GLuint s_overlayTex;
static int s_overlayW = 0, s_overlayH = 0;
static GLuint s_overlayProg;
static GLint s_overlayRect, s_overlayWindow;
static GXPCStats s_lastFrameStats;
static bool s_ready = false;
static bool s_xfDirty = true;

// Append-only stream of per-draw data (vertices, indices, the XF block) in
// one GL buffer, a batch's pieces written through one unsynchronized map. The
// buffer is split into four segments; a fence marks the end of each one's use,
// and a segment is only written again once its fence has passed, so a write
// never touches data a queued draw still reads. Replaces a glBufferData
// reallocation (or a glBufferSubData into a buffer every queued draw uses) per
// batch, and a map per piece (map/unmap were half of all GL calls).
struct StreamPiece {
    const void* data;
    size_t bytes, align;
    size_t at;  // out: offset written at, a multiple of align
};
struct StreamBuffer {
    enum { kSegments = 4 };
    GLuint buf = 0;
    size_t size = 0, pos = 0;
    GLsync fence[kSegments] = {};

    void init(size_t bytes) {
        size = bytes;
        glGenBuffers(1, &buf);
        glcBindArrayBuffer(buf);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size), nullptr, GL_STREAM_DRAW);
    }
    size_t unfenced = 0;  // first segment written since the last fence

    size_t segOf(size_t p) const { return p * kSegments / size; }
    // Every draw reading what was written so far has been issued: fence the
    // segments from `unfenced` through `last`.
    void fenceThrough(size_t last) {
        for (size_t sg = unfenced; sg <= last && sg < kSegments; sg++)
            if (!fence[sg]) fence[sg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    void waitSegment(size_t seg) {
        if (!fence[seg]) return;
        GxTimer tw(&s_waitSeconds);
        glClientWaitSync(fence[seg], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(fence[seg]);
        fence[seg] = 0;
    }
    static size_t worstCase(const StreamPiece* pc, int n) {
        size_t w = 0;
        for (int i = 0; i < n; i++) w += pc[i].bytes + pc[i].align;
        return w;
    }
    // appendAll of pieces this large starts over at the buffer's beginning
    bool wouldWrap(size_t worst) const { return pos + worst > size; }
    // Writes the pieces one after another (each at a multiple of its
    // alignment) and sets their offsets. Called before the draw that reads
    // them, after the previous one.
    void appendAll(StreamPiece* pc, int n) {
        size_t start = wouldWrap(worstCase(pc, n)) ? 0 : pos;
        size_t end = start;
        for (int i = 0; i < n; i++) {
            pc[i].at = (end + pc[i].align - 1) / pc[i].align * pc[i].align;
            end = pc[i].at + pc[i].bytes;
        }
        size_t first = pc[0].at;
        size_t cur = segOf(pos ? pos - 1 : 0);
        if (start == 0 && pos != 0) {  // wrap
            fenceThrough(kSegments - 1);
            unfenced = 0;
        } else if (segOf(first) != cur) {
            fenceThrough(segOf(first) - 1);
            unfenced = segOf(first);
        }
        for (size_t sg = segOf(first), last = segOf(end - 1); sg <= last; sg++) waitSegment(sg);
        glcBindArrayBuffer(buf);
        uint8_t* dst = static_cast<uint8_t*>(
            glMapBufferRange(GL_ARRAY_BUFFER, GLintptr(first), GLsizeiptr(end - first),
                             GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
        if (dst) {
            for (int i = 0; i < n; i++) memcpy(dst + (pc[i].at - first), pc[i].data, pc[i].bytes);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        pos = end;
    }
};
static StreamBuffer s_stream;
static GLint s_uboAlign = 256;

// One VAO per packed vertex format: attribute pointers into the vertex stream
// for the attributes the format holds; the others read the constant values
// set in rendererInit (matrix indices: set per batch in flushBatch).
static std::unordered_map<uint32_t, GLuint> s_fmtVaos;

static GLuint vaoForFormat(uint32_t fmt) {
    GLuint& vao = s_fmtVaos[fmt];
    if (vao) return vao;
    const VtxFmtLayout& l = vtxFmtLayout(fmt);
    glGenVertexArrays(1, &vao);
    glcBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_stream.buf);
    glcBindArrayBuffer(s_stream.buf);
    const GLsizei st = l.stride;
    auto off = [](size_t o) { return reinterpret_cast<const void*>(o); };
    auto attr = [&](GLuint i, bool on, GLint n, GLenum type, GLboolean norm, size_t o) {
        if (!on) return;
        glVertexAttribPointer(i, n, type, norm, st, off(o));
        glEnableVertexAttribArray(i);
    };
    attr(0, true, 3, GL_FLOAT, GL_FALSE, 0);
    attr(1, fmt & VF_NRM, 3, GL_FLOAT, GL_FALSE, l.nrm);
    attr(2, (fmt & VF_NRM) && (fmt & VF_NBT), 3, GL_FLOAT, GL_FALSE, l.nrm + 12);
    attr(3, (fmt & VF_NRM) && (fmt & VF_NBT), 3, GL_FLOAT, GL_FALSE, l.nrm + 24);
    attr(4, fmt & VF_CLR0, 4, GL_UNSIGNED_BYTE, GL_TRUE, l.clr[0]);
    attr(5, fmt & (VF_CLR0 << 1), 4, GL_UNSIGNED_BYTE, GL_TRUE, l.clr[1]);
    for (int t = 0; t < 8; t++) attr(GLuint(6 + t), fmt & (VF_TEX0 << t), 2, GL_FLOAT, GL_FALSE, l.tex[t]);
    if (fmt & VF_MTX) {
        glVertexAttribIPointer(14, 3, GL_UNSIGNED_INT, st, off(l.mtx));
        glEnableVertexAttribArray(14);
    }
    return vao;
}

// The batch: packed vertices and their indices. Plain growable arrays rather
// than vectors, so growing does not zero-fill and an index is one store.
template <typename T> struct GrowBuf {
    T* data = nullptr;
    size_t size = 0, cap = 0;
    T* reserve(size_t more) {  // room for `more` past size; returns the end
        if (size + more > cap) {
            size_t c = cap ? cap : 4096;
            while (c < size + more) c *= 2;
            T* d = static_cast<T*>(realloc(data, c * sizeof(T)));
            if (!d) fatal("out of memory growing a draw batch");
            data = d;
            cap = c;
        }
        return data + size;
    }
};
static GrowBuf<uint8_t> s_bdata;  // the batch's packed vertices
static uint32_t s_bcount = 0, s_bfmt = 0, s_bstride = 12;
static GrowBuf<uint32_t> s_bidx;
static PrimClass s_bclass = PRIM_TRIS;

struct Xfb {
    GLuint tex;
    int w, h;
};
static std::unordered_map<const void*, Xfb> s_xfbs;
static const void* s_lastXfb = nullptr;

void markXfMemDirty() { s_xfDirty = true; }
FILE* traceFile();
void traceFrameAdvance();
void traceDraw(int prim, uint32_t nverts, uint32_t nidx, const HostVertex* v, int progId);
void traceCopy(bool disp, int x, int y, int w, int h, const void* dest, uint32_t ctrl);
void traceProbe();
void (*g_displayCopyHook)(const void* xfb) = nullptr;
bool rendererReady() { return s_ready; }

static GLuint compileProgram(const char* vs, const char* fs) {
    auto sh = [](GLenum t, const char* src) {
        GLuint s = glCreateShader(t);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            logmsg("internal shader failed: %s", log);
        }
        return s;
    };
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindFragDataLocation(p, 0, "o_color");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// EFB -> texture conversion.  The output is what sampling the GX texture of
// the copy's format would return, so later draws can use it directly.
static const char* kCopyVs = R"(#version 330 core
uniform vec4 u_rect; // x, y, w, h in EFB texture coordinates (0..1)
out vec2 v_uv;
void main() {
  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  v_uv = u_rect.xy + p * u_rect.zw;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kCopyFs = R"(#version 330 core
uniform sampler2D u_color;
uniform sampler2D u_depth;
uniform int u_mode;     // bit 5: depth copy, bit 4: intensity (YUV) conversion, bits 0-3: format
uniform int u_alphaOne; // EFB pixel format has no alpha
in vec2 v_uv;
out vec4 o_color;
float q(float v, float bits) { float m = exp2(bits) - 1.0; return floor(v * m + 0.5) / m; }
void main() {
  int f = u_mode & 15;
  if ((u_mode & 32) != 0) {
    uint z = uint(clamp(texture(u_depth, v_uv).r, 0.0, 1.0) * 16777215.0);
    float hi = float((z >> 16) & 255u) / 255.0, mid = float((z >> 8) & 255u) / 255.0, lo = float(z & 255u) / 255.0;
    if (f == 1) o_color = vec4(hi);                        // Z8
    else if (f == 11 || f == 3) o_color = vec4(mid, mid, mid, hi); // Z16 (IA8 layout)
    else if (f == 6) o_color = vec4(hi, mid, lo, 1.0);     // Z24X8
    else if (f == 0) o_color = vec4(q(hi, 4.0));           // Z4
    else if (f == 9) o_color = vec4(mid);                  // Z8M
    else if (f == 10) o_color = vec4(lo);                  // Z8L
    else if (f == 12) o_color = vec4(lo, lo, lo, mid);     // Z16L
    else o_color = vec4(hi);
    return;
  }
  vec4 c = texture(u_color, v_uv);
  if (u_alphaOne != 0) c.a = 1.0;
  if ((u_mode & 16) != 0) {
    float y = clamp(0.257 * c.r + 0.504 * c.g + 0.098 * c.b + 16.0 / 255.0, 0.0, 1.0);
    if (f == 0) o_color = vec4(q(y, 4.0));
    else if (f == 1) o_color = vec4(y);
    else if (f == 2) o_color = vec4(vec3(q(y, 4.0)), q(c.a, 4.0));
    else if (f == 3) o_color = vec4(vec3(y), c.a);
    else o_color = c;
    return;
  }
  if (f == 0) o_color = vec4(q(c.r, 4.0));                                // R4
  else if (f == 2) o_color = vec4(vec3(q(c.r, 4.0)), q(c.a, 4.0));        // RA4
  else if (f == 3) o_color = vec4(c.rrr, c.a);                            // RA8
  else if (f == 4) o_color = vec4(q(c.r, 5.0), q(c.g, 6.0), q(c.b, 5.0), 1.0); // RGB565
  else if (f == 5) o_color = c.a > 0.99 ? vec4(q(c.r, 5.0), q(c.g, 5.0), q(c.b, 5.0), 1.0)
                                        : vec4(q(c.r, 4.0), q(c.g, 4.0), q(c.b, 4.0), q(c.a, 3.0)); // RGB5A3
  else if (f == 7) o_color = vec4(c.a);                                   // A8
  else if (f == 8) o_color = vec4(c.r);                                   // R8
  else if (f == 9) o_color = vec4(c.g);                                   // G8
  else if (f == 10) o_color = vec4(c.b);                                  // B8
  else if (f == 11) o_color = vec4(c.ggg, c.r);                           // RG8 read as IA8
  else if (f == 12) o_color = vec4(c.bbb, c.g);                           // GB8 read as IA8
  else o_color = c;                                                       // RGBA8 / YUVA8
}
)";

// ------------------------------------------------------------ post-processing
// The XFB reaches the window through up to two passes: FXAA at the XFB's own
// size (SMS_FXAA), then a scaling pass into the letterboxed viewport that also
// sharpens (SMS_SHARPEN) and applies the brightness curve (SMS_GAMMA).
// SMS_PRESENT_FILTER picks the scaler: bilinear (an area average when the
// XFB is larger than the window, so a high internal resolution supersamples),
// nearest, or sharp (bilinear only between texels: crisp pixels at any size).
// SMS_ASPECT=stretch fills the window; integer keeps whole multiples of 640x528.
static const char* kPostVs = R"(#version 330 core
uniform int u_flip;
out vec2 v_uv;
void main() {
  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  v_uv = u_flip != 0 ? vec2(p.x, 1.0 - p.y) : p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kFxaaFs = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_rcp;  // 1 / texture size
in vec2 v_uv;
out vec4 o_color;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
  vec3 nw = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_rcp).rgb;
  vec3 ne = texture(u_tex, v_uv + vec2( 1.0, -1.0) * u_rcp).rgb;
  vec3 sw = texture(u_tex, v_uv + vec2(-1.0,  1.0) * u_rcp).rgb;
  vec3 se = texture(u_tex, v_uv + vec2( 1.0,  1.0) * u_rcp).rgb;
  vec3 m  = texture(u_tex, v_uv).rgb;
  float lNW = luma(nw), lNE = luma(ne), lSW = luma(sw), lSE = luma(se), lM = luma(m);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
  float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * u_rcp;
  vec3 a = 0.5 * (texture(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                  texture(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 b = a * 0.5 + 0.25 * (texture(u_tex, v_uv - dir * 0.5).rgb + texture(u_tex, v_uv + dir * 0.5).rgb);
  float lB = luma(b);
  o_color = vec4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)";
static const char* kScaleFs = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_src;      // texture size in texels
uniform vec2 u_dst;      // viewport size in pixels
uniform int u_filter;    // 0 bilinear/area, 1 nearest, 2 sharp
uniform float u_sharpen; // 0..1
uniform float u_gamma;   // 1: unchanged; above 1 brightens
in vec2 v_uv;
out vec4 o_color;
vec3 areaSample(vec2 uv) {
  vec2 ratio = u_src / u_dst;  // texels per pixel
  if (ratio.x <= 1.0 && ratio.y <= 1.0) return texture(u_tex, uv).rgb;
  ivec2 n = ivec2(clamp(ceil(ratio), vec2(1.0), vec2(8.0)));
  vec2 stepUv = ratio / vec2(n) / u_src;
  vec2 start = uv - 0.5 * ratio / u_src + 0.5 * stepUv;
  vec3 acc = vec3(0.0);
  for (int j = 0; j < n.y; j++)
    for (int i = 0; i < n.x; i++) acc += texture(u_tex, start + vec2(i, j) * stepUv).rgb;
  return acc / float(n.x * n.y);
}
vec3 fetch(vec2 uv) {
  if (u_filter == 1) return texelFetch(u_tex, clamp(ivec2(uv * u_src), ivec2(0), ivec2(u_src) - 1), 0).rgb;
  if (u_filter == 2 && u_dst.x >= u_src.x) {
    vec2 scale = max(floor(u_dst / u_src), vec2(1.0));
    vec2 texel = uv * u_src;
    vec2 base = floor(texel - 0.5) + 0.5;
    vec2 f = texel - base;
    vec2 region = 0.5 - 0.5 / scale;
    vec2 d = f - 0.5;
    f = (d - clamp(d, -region, region)) * scale + 0.5;
    return texture(u_tex, (base + f) / u_src).rgb;
  }
  return areaSample(uv);
}
void main() {
  vec3 c = fetch(v_uv);
  if (u_sharpen > 0.0) {
    // contrast-adaptive: sharpen less where the neighbourhood already has contrast
    vec2 d = max(1.0 / u_src, 1.0 / u_dst);
    vec3 n = texture(u_tex, v_uv + vec2(0.0, -d.y)).rgb, s = texture(u_tex, v_uv + vec2(0.0, d.y)).rgb;
    vec3 w = texture(u_tex, v_uv + vec2(-d.x, 0.0)).rgb, e = texture(u_tex, v_uv + vec2(d.x, 0.0)).rgb;
    vec3 mn = min(c, min(min(n, s), min(w, e))), mx = max(c, max(max(n, s), max(w, e)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    vec3 wgt = -amp * mix(0.125, 0.2, u_sharpen);
    c = clamp((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
  }
  if (u_gamma != 1.0) c = pow(max(c, vec3(0.0)), vec3(1.0 / u_gamma));
  o_color = vec4(c, 1.0);
}
)";

struct PostSettings {
    bool fxaa = false;
    int filter = 0;        // 0 bilinear/area, 1 nearest, 2 sharp
    float sharpen = 0.0f;  // 0..1
    float gamma = 1.0f;
    int aspect = 0;        // 0 keep, 1 stretch, 2 integer
};
static PostSettings s_post;
static GLuint s_fxaaProg, s_scaleProg, s_postTex, s_postFbo;
static GLint s_fxaaURcp, s_fxaaUFlip, s_scaleUSrc, s_scaleUDst, s_scaleUFilter, s_scaleUSharpen, s_scaleUGamma,
    s_scaleUFlip;
static int s_postW, s_postH;

static void postInit() {
    if (const char* e = getenv("SMS_FXAA")) s_post.fxaa = atoi(e) != 0;
    if (const char* e = getenv("SMS_PRESENT_FILTER"))
        s_post.filter = !strcmp(e, "nearest") ? 1 : !strcmp(e, "sharp") ? 2 : 0;
    if (const char* e = getenv("SMS_SHARPEN")) s_post.sharpen = std::max(0.0f, std::min(1.0f, float(atof(e)) / 100.0f));
    if (const char* e = getenv("SMS_GAMMA")) {
        float v = float(atof(e));
        if (v >= 0.3f && v <= 3.0f) s_post.gamma = v;
    }
    if (const char* e = getenv("SMS_ASPECT")) s_post.aspect = !strcmp(e, "stretch") ? 1 : !strcmp(e, "integer") ? 2 : 0;
    s_fxaaProg = compileProgram(kPostVs, kFxaaFs);
    glUseProgram(s_fxaaProg);
    glUniform1i(glGetUniformLocation(s_fxaaProg, "u_tex"), 0);
    s_fxaaURcp = glGetUniformLocation(s_fxaaProg, "u_rcp");
    s_fxaaUFlip = glGetUniformLocation(s_fxaaProg, "u_flip");
    s_scaleProg = compileProgram(kPostVs, kScaleFs);
    glUseProgram(s_scaleProg);
    glUniform1i(glGetUniformLocation(s_scaleProg, "u_tex"), 0);
    s_scaleUSrc = glGetUniformLocation(s_scaleProg, "u_src");
    s_scaleUDst = glGetUniformLocation(s_scaleProg, "u_dst");
    s_scaleUFilter = glGetUniformLocation(s_scaleProg, "u_filter");
    s_scaleUSharpen = glGetUniformLocation(s_scaleProg, "u_sharpen");
    s_scaleUGamma = glGetUniformLocation(s_scaleProg, "u_gamma");
    s_scaleUFlip = glGetUniformLocation(s_scaleProg, "u_flip");
    static const char* const kFilters[] = {"bilinear", "nearest", "sharp"};
    static const char* const kAspects[] = {"keep", "stretch", "integer"};
    logmsg("post-processing: FXAA %s, scaler %s, sharpen %d%%, brightness %.2f, aspect %s", s_post.fxaa ? "on" : "off",
           kFilters[s_post.filter], int(s_post.sharpen * 100.0f + 0.5f), double(s_post.gamma), kAspects[s_post.aspect]);
}

// Draws `tex` (w x h, row 0 at the top) into the viewport (ox, oy, vw, vh) of
// framebuffer 0, through FXAA when it is on.
static void postPresent(GLuint tex, int w, int h, int ox, int oy, int vw, int vh) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE0 + 1);
    glDisable(GL_SCISSOR_TEST);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_copyVao);
    if (s_post.fxaa) {
        if (!s_postTex || s_postW != w || s_postH != h) {
            if (!s_postTex) glGenTextures(1, &s_postTex);
            glBindTexture(GL_TEXTURE_2D, s_postTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (!s_postFbo) glGenFramebuffers(1, &s_postFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s_postFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_postTex, 0);
            s_postW = w;
            s_postH = h;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, s_postFbo);
        glViewport(0, 0, w, h);
        glUseProgram(s_fxaaProg);
        glUniform2f(s_fxaaURcp, 1.0f / float(w), 1.0f / float(h));
        glUniform1i(s_fxaaUFlip, 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        tex = s_postTex;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(ox, oy, vw, vh);
    glUseProgram(s_scaleProg);
    glUniform2f(s_scaleUSrc, float(w), float(h));
    glUniform2f(s_scaleUDst, float(vw), float(vh));
    glUniform1i(s_scaleUFilter, s_post.filter);
    glUniform1f(s_scaleUSharpen, s_post.sharpen);
    glUniform1f(s_scaleUGamma, s_post.gamma);
    glUniform1i(s_scaleUFlip, 1);  // XFB row 0 is the top; the window's row 0 is its bottom
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(s_vao);
}

void rendererInit(int efbScale) {
    s_scale = efbScale < 1 ? 1 : efbScale;
    g_gxStats = g_gxStats || statsEnv();  // settings.txt is read after static initialisation
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    logmsg("OpenGL %s, renderer %s (%s)", (const char*)glGetString(GL_VERSION), renderer,
           (const char*)glGetString(GL_VENDOR));
    if (renderer && (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") || strstr(renderer, "Software")))
        logmsg("WARNING: %s renders on the CPU and cannot keep the game at full speed. On Linux the 32-bit build "
               "gets a GPU driver only if its 32-bit GL libraries are installed; the 64-bit build "
               "(SMS_ARCH=64 ./build.sh) uses the system's driver.", renderer);
    s_efbW = s_wide > 1.0f ? (int(float(EFB_W) * s_wide + 1.0f) & ~1) : EFB_W;
    if (const char* e = getenv("SMS_WIDESCREEN_HUD")) s_hudEdges = s_efbW != EFB_W && !strcmp(e, "edges");
    s_ox = (s_efbW - EFB_W) / 2;
    if (s_efbW != EFB_W) logmsg("widescreen: EFB %dx%d", s_efbW, EFB_H);
    int W = s_efbW * s_scale, H = EFB_H * s_scale;
    glGenTextures(1, &s_efbColor);
    glBindTexture(GL_TEXTURE_2D, s_efbColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &s_efbDepth);
    glBindTexture(GL_TEXTURE_2D, s_efbDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, W, H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (const char* e = getenv("SMS_MSAA")) s_msaa = atoi(e);
    if (s_msaa > 1) {
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        s_msaa = std::min(s_msaa, int(maxSamples));
    }
    if (s_msaa < 2) s_msaa = 0;
    glGenFramebuffers(1, &s_efbFbo);
    if (s_msaa) {
        glGenRenderbuffers(1, &s_efbMsColor);
        glBindRenderbuffer(GL_RENDERBUFFER, s_efbMsColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_RGBA8, W, H);
        glGenRenderbuffers(1, &s_efbMsDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, s_efbMsDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_DEPTH_COMPONENT24, W, H);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &s_efbResolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_efbResolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_efbColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_efbDepth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logmsg("EFB resolve framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_efbMsColor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_efbMsDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            logmsg("anti-aliasing: %dx MSAA", s_msaa);
        } else {  // fall back to the plain EFB
            logmsg("anti-aliasing: %dx MSAA framebuffer incomplete, MSAA off", s_msaa);
            s_msaa = 0;
        }
    }
    if (!s_msaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_efbColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_efbDepth, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logmsg("EFB framebuffer incomplete");
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glGenFramebuffers(1, &s_tmpFbo);

    glcInvalidate();
    glGenVertexArrays(1, &s_vao);
    glcBindVertexArray(s_vao);
    s_stream.init(128u << 20);
    // Values of the attributes a packed format leaves out (see vaoForFormat):
    // the same defaults the loader used to write into every vertex.
    glVertexAttrib4f(1, 0.0f, 0.0f, 1.0f, 1.0f);  // normal
    glVertexAttrib4f(2, 0.0f, 0.0f, 0.0f, 1.0f);  // binormal
    glVertexAttrib4f(3, 0.0f, 0.0f, 0.0f, 1.0f);  // tangent
    glVertexAttrib4f(4, 1.0f, 1.0f, 1.0f, 1.0f);  // colour 0
    glVertexAttrib4f(5, 1.0f, 1.0f, 1.0f, 1.0f);  // colour 1
    for (GLuint i = 6; i < 14; i++) glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 1.0f);  // texcoords

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_uboAlign);
    if (s_uboAlign < 16) s_uboAlign = 16;

    s_copyProg = compileProgram(kCopyVs, kCopyFs);
    glUseProgram(s_copyProg);
    glUniform1i(glGetUniformLocation(s_copyProg, "u_color"), 0);
    glUniform1i(glGetUniformLocation(s_copyProg, "u_depth"), 1);
    s_copyUMode = glGetUniformLocation(s_copyProg, "u_mode");
    s_copyURect = glGetUniformLocation(s_copyProg, "u_rect");
    s_copyUAlphaOne = glGetUniformLocation(s_copyProg, "u_alphaOne");
    glGenVertexArrays(1, &s_copyVao);
    postInit();
    glcInvalidate();
    s_ready = true;
}

// ------------------------------------------------------------------ batching
void onStateChange() {
    if (s_bidx.size) flushBatch();
}

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride) {
    PrimClass cls = op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS;
    if (s_bcount && (cls != s_bclass || fmt != s_bfmt)) flushBatch();
    s_bclass = cls;
    s_bfmt = fmt;
    s_bstride = stride;
    s_bdata.size = size_t(s_bcount) * stride;
    return s_bdata.reserve(size_t(n) * stride);
}

void primitiveEnd(uint8_t op, uint32_t n) {
    if (!s_ready || n == 0) return;
    uint32_t base = s_bcount;
    s_bcount += n;
    s_bdata.size = size_t(s_bcount) * s_bstride;
    uint32_t* w = s_bidx.reserve(size_t(n) * 3);  // no primitive makes more than 3 per vertex
    uint32_t* const w0 = w;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        w[0] = base + a;
        w[1] = base + b;
        w[2] = base + c;
        w += 3;
    };
    switch (op & 0xF8) {
    case 0x80:
    case 0x88:  // quads
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
        break;
    case 0x90:
        for (uint32_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2);
        break;
    case 0x98:
        for (uint32_t i = 2; i < n; i++) {
            if (i & 1) tri(i - 1, i - 2, i);
            else tri(i - 2, i - 1, i);
        }
        break;
    case 0xA0:
        for (uint32_t i = 2; i < n; i++) tri(0, i - 1, i);
        break;
    case 0xA8:
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            *w++ = base + i;
            *w++ = base + i + 1;
        }
        break;
    case 0xB0:
        for (uint32_t i = 1; i < n; i++) {
            *w++ = base + i - 1;
            *w++ = base + i;
        }
        break;
    default:
        for (uint32_t i = 0; i < n; i++) *w++ = base + i;
        break;
    }
    s_bidx.size += size_t(w - w0);
    s_stats.vertices += n;
}

static inline int sext11(uint32_t v) { return int32_t(v << 21) >> 21; }
static inline float xff(uint32_t r) {
    float f;
    memcpy(&f, &g.xfReg[r], 4);
    return f;
}

GlCache g_glc;

void glcInvalidate() {
    // raw paths sample with the textures' own parameters: drop the samplers
    for (int u = 0; u < 8; u++)
        if (g_glc.sampler[u] != 0 && g_glc.sampler[u] != ~0u) glBindSampler(GLuint(u), 0);
    memset(&g_glc, 0xFF, sizeof(g_glc));  // ~0 names/enums, -1 flags, NaN floats
}

void glcForgetTexture(GLuint tex) {
    for (int u = 0; u < 8; u++)
        if (g_glc.tex[u] == tex) g_glc.tex[u] = ~0u;
}

static void applyGlState() {
    GlCache& c = g_glc;
    int W = s_efbW * s_scale, H = EFB_H * s_scale;
    if (c.fbo != s_efbFbo) {
        c.fbo = s_efbFbo;
        glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
    }
    if (c.vp[0] != 0 || c.vp[1] != 0 || c.vp[2] != W || c.vp[3] != H) {
        c.vp[0] = c.vp[1] = 0;
        c.vp[2] = W;
        c.vp[3] = H;
        glViewport(0, 0, W, H);
    }

    // scissor (registers hold coordinates + 342)
    uint32_t tl = g.bp[BP_SCISSOR_TL], br = g.bp[BP_SCISSOR_BR];
    int top = int(tl & 0x7FF) - 342, left = int((tl >> 12) & 0x7FF) - 342;
    int bottom = int(br & 0x7FF) - 342, right = int((br >> 12) & 0x7FF) - 342;
    int sw = right - left + 1, sh = bottom - top + 1;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    glcCap(GL_SCISSOR_TEST, c.scissor, true);
    if (s_efbW != EFB_W) {  // into the draw's widescreen mapping; the whole width stays whole
        if (left <= 0 && left + sw >= EFB_W) {
            left = 0;
            sw = s_efbW;
        } else {
            int l = int(floorf(float(left) * s_xmap.a + s_xmap.b)), r = int(ceilf(float(left + sw) * s_xmap.a + s_xmap.b));
            left = l;
            sw = r - l;
        }
    }
    GLint sc[4] = {left * s_scale, top * s_scale, sw * s_scale, sh * s_scale};
    if (memcmp(sc, c.sc, sizeof sc) != 0) {
        memcpy(c.sc, sc, sizeof sc);
        glScissor(sc[0], sc[1], sc[2], sc[3]);
    }

    // culling: GX front faces are clockwise on screen, which is counter-clockwise
    // in this framebuffer's (y-down) window coordinates
    uint32_t cull = (g.bp[BP_GENMODE] >> 14) & 3;
    if (c.frontFace != GL_CCW) {
        c.frontFace = GL_CCW;
        glFrontFace(GL_CCW);
    }
    if (cull == 0 || s_bclass != PRIM_TRIS) glcCap(GL_CULL_FACE, c.cull, false);
    else {
        glcCap(GL_CULL_FACE, c.cull, true);
        GLenum cf = cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK;
        if (c.cullFace != cf) {
            c.cullFace = cf;
            glCullFace(cf);
        }
    }

    static const GLenum cmp[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    uint32_t z = g.bp[BP_ZMODE];
    GLboolean dmask = GL_FALSE;
    if (z & 1) {
        glcCap(GL_DEPTH_TEST, c.depth, true);
        GLenum df = cmp[(z >> 1) & 7];
        if (c.depthFunc != df) {
            c.depthFunc = df;
            glDepthFunc(df);
        }
        dmask = (z >> 4) & 1 ? GL_TRUE : GL_FALSE;
    } else {
        glcCap(GL_DEPTH_TEST, c.depth, false);
    }
    if (c.depthMask != dmask) {
        c.depthMask = dmask;
        glDepthMask(dmask);
    }

    uint32_t pix = g.bp[BP_PE_CONTROL] & 7;
    bool hasAlpha = pix == 1;  // GX_PF_RGBA6_Z24
    uint32_t cm = g.bp[BP_CMODE0], cm1 = g.bp[BP_CMODE1];
    bool colorUpd = (cm >> 3) & 1, alphaUpd = ((cm >> 4) & 1) && hasAlpha;
    GLboolean mask[4] = {GLboolean(colorUpd), GLboolean(colorUpd), GLboolean(colorUpd),
                         GLboolean(alphaUpd || ((cm1 >> 8) & 1 && hasAlpha))};
    if (memcmp(mask, c.cmask, sizeof mask) != 0) {
        memcpy(c.cmask, mask, sizeof mask);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
    }

    static const GLenum srcF[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                   GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    static const GLenum dstF[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                   GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    auto noDstAlpha = [&](GLenum f) {
        if (hasAlpha) return f;
        return f == GL_DST_ALPHA ? GLenum(GL_ONE) : f == GL_ONE_MINUS_DST_ALPHA ? GLenum(GL_ZERO) : f;
    };
    auto blendEq = [&](GLenum rgb, GLenum a) {
        if (c.beq[0] != rgb || c.beq[1] != a) {
            c.beq[0] = rgb;
            c.beq[1] = a;
            glBlendEquationSeparate(rgb, a);
        }
    };
    auto blendFunc = [&](GLenum s0, GLenum d0, GLenum s1, GLenum d1) {
        if (c.bf[0] != s0 || c.bf[1] != d0 || c.bf[2] != s1 || c.bf[3] != d1) {
            c.bf[0] = s0;
            c.bf[1] = d0;
            c.bf[2] = s1;
            c.bf[3] = d1;
            glBlendFuncSeparate(s0, d0, s1, d1);
        }
    };
    bool dstAlpha = ((cm1 >> 8) & 1) != 0;
    GLenum aSrc = GL_ONE, aDst = GL_ZERO;
    if (dstAlpha) {
        float bc = float(cm1 & 0xFF) / 255.0f;
        if (!(c.blendColor[3] == bc) || !(c.blendColor[0] == 0.0f)) {
            c.blendColor[0] = c.blendColor[1] = c.blendColor[2] = 0.0f;
            c.blendColor[3] = bc;
            glBlendColor(0, 0, 0, bc);
        }
        aSrc = GL_CONSTANT_ALPHA;
        aDst = GL_ZERO;
    }
    if (cm & 1) {
        glcCap(GL_COLOR_LOGIC_OP, c.logic, false);
        glcCap(GL_BLEND, c.blend, true);
        if ((cm >> 11) & 1) {
            blendEq(GL_FUNC_REVERSE_SUBTRACT, dstAlpha ? GL_FUNC_ADD : GL_FUNC_REVERSE_SUBTRACT);
            blendFunc(GL_ONE, GL_ONE, dstAlpha ? aSrc : GL_ONE, dstAlpha ? aDst : GL_ONE);
        } else {
            GLenum s = noDstAlpha(srcF[(cm >> 8) & 7]), d = noDstAlpha(dstF[(cm >> 5) & 7]);
            blendEq(GL_FUNC_ADD, GL_FUNC_ADD);
            blendFunc(s, d, dstAlpha ? aSrc : s, dstAlpha ? aDst : d);
        }
    } else if ((cm >> 1) & 1) {
        static const GLenum lop[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP,
                                       GL_XOR, GL_OR, GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE,
                                       GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
        glcCap(GL_BLEND, c.blend, false);
        glcCap(GL_COLOR_LOGIC_OP, c.logic, true);
        GLenum op = lop[(cm >> 12) & 15];
        if (c.logicOp != op) {
            c.logicOp = op;
            glLogicOp(op);
        }
    } else if (dstAlpha) {
        glcCap(GL_COLOR_LOGIC_OP, c.logic, false);
        glcCap(GL_BLEND, c.blend, true);
        blendEq(GL_FUNC_ADD, GL_FUNC_ADD);
        blendFunc(GL_ONE, GL_ZERO, aSrc, aDst);
    } else {
        glcCap(GL_COLOR_LOGIC_OP, c.logic, false);
        glcCap(GL_BLEND, c.blend, false);
    }

    bool clipOn = g.xfReg[0x05] == 0;
    glcCap(GL_CLIP_DISTANCE0, c.clip0, clipOn);
    glcCap(GL_CLIP_DISTANCE0 + 1, c.clip1, clipOn);
    uint32_t lp = g.bp[BP_LPSIZE];
    float pt = float((lp >> 8) & 0xFF) / 6.0f * float(s_scale);
    if (pt < 1.0f) pt = 1.0f;
    if (!(c.pointSize == pt)) {
        c.pointSize = pt;
        glPointSize(pt);
    }
    if (!(c.lineWidth == 1.0f)) {
        c.lineWidth = 1.0f;
        glLineWidth(1.0f);
    }
}

static void uploadUniforms(const ShaderProgram* sp, float texW[8], float texH[8]) {
    UniformCache& uc = sp->uc;
    // uploads `value` (same size as the cached copy) only when it changed
#define UNI(loc, field, value, call)                                               \
    if (!uc.init || memcmp(uc.field, value, sizeof uc.field) != 0) {                \
        memcpy(uc.field, value, sizeof uc.field);                                   \
        if (sp->loc >= 0) call;                                                     \
    }
    GLint iv[16];
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        iv[4 * i + 0] = sext11(ra & 0x7FF);
        iv[4 * i + 1] = sext11((bg >> 12) & 0x7FF);
        iv[4 * i + 2] = sext11(bg & 0x7FF);
        iv[4 * i + 3] = sext11((ra >> 12) & 0x7FF);
    }
    UNI(uTevReg, tevreg, iv, glUniform4iv(sp->uTevReg, 4, iv));
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
        iv[4 * i + 0] = int(ra & 0xFF);
        iv[4 * i + 1] = int((bg >> 12) & 0xFF);
        iv[4 * i + 2] = int(bg & 0xFF);
        iv[4 * i + 3] = int((ra >> 12) & 0xFF);
    }
    UNI(uKonst, konst, iv, glUniform4iv(sp->uKonst, 4, iv));

    // manual texcoord scaling: coordinates are in units of the given size
    float ts[16];
    for (int c = 0; c < 8; c++) ts[2 * c] = ts[2 * c + 1] = 1.0f;
    uint32_t nst = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
    for (uint32_t s = 0; s < nst; s++) {
        uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        uint32_t map = ord & 7, coord = (ord >> 3) & 7;
        if (g.tcManual[coord] && texW[map] > 0) {
            ts[2 * coord] = float((g.bp[BP_SU_SSIZE + 2 * coord] & 0xFFFF) + 1) / texW[map];
            ts[2 * coord + 1] = float((g.bp[BP_SU_SSIZE + 2 * coord + 1] & 0xFFFF) + 1) / texH[map];
        }
    }
    UNI(uTexScale, texscale, ts, glUniform2fv(sp->uTexScale, 8, ts));
    float tsz[16];
    for (int m = 0; m < 8; m++) {
        tsz[2 * m] = texW[m] > 0 ? texW[m] : 1.0f;
        tsz[2 * m + 1] = texH[m] > 0 ? texH[m] : 1.0f;
    }
    UNI(uTexSize, texsize, tsz, glUniform2fv(sp->uTexSize, 8, tsz));
    uint32_t ac = g.bp[BP_ALPHACOMPARE];
    GLint ar[2] = {GLint(ac & 0xFF), GLint((ac >> 8) & 0xFF)};
    UNI(uAlphaRef, alpharef, ar, glUniform2iv(sp->uAlphaRef, 1, ar));

    // fog
    uint32_t f0 = g.bp[BP_FOG0], f1 = g.bp[BP_FOG1], f2 = g.bp[BP_FOG2], f3 = g.bp[BP_FOG3];
    auto fogFloat = [](uint32_t r) {
        uint32_t bits = ((r >> 19) & 1) << 31 | ((r >> 11) & 0xFF) << 23 | (r & 0x7FF) << 12;
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    float a = fogFloat(f0), c = fogFloat(f3);
    int bs = int(f2 & 31);
    float A = ldexpf(a, bs);
    float B = ldexpf(float(f1 & 0xFFFFFF) / 8388638.0f, bs - 1);
    float fog[4] = {A, B, c, float((f3 >> 20) & 1)};
    UNI(uFog, fog, fog, glUniform4fv(sp->uFog, 1, fog));
    uint32_t fc = g.bp[BP_FOG_COLOR];
    float fcol[4] = {float((fc >> 16) & 255) / 255.0f, float((fc >> 8) & 255) / 255.0f, float(fc & 255) / 255.0f, 1.0f};
    UNI(uFogColor, fogcolor, fcol, glUniform4fv(sp->uFogColor, 1, fcol));

    float im[24];
    for (int m = 0; m < 3; m++) {
        uint32_t r0 = g.bp[BP_IND_MTX + 3 * m], r1 = g.bp[BP_IND_MTX + 3 * m + 1], r2 = g.bp[BP_IND_MTX + 3 * m + 2];
        int sc = int(((r0 >> 22) & 3) | ((r1 >> 22) & 3) << 2 | ((r2 >> 22) & 3) << 4) - 17;
        float* row0 = &im[8 * m];
        float* row1 = &im[8 * m + 4];
        row0[0] = sext11(r0 & 0x7FF) / 1024.0f;
        row1[0] = sext11((r0 >> 11) & 0x7FF) / 1024.0f;
        row0[1] = sext11(r1 & 0x7FF) / 1024.0f;
        row1[1] = sext11((r1 >> 11) & 0x7FF) / 1024.0f;
        row0[2] = sext11(r2 & 0x7FF) / 1024.0f;
        row1[2] = sext11((r2 >> 11) & 0x7FF) / 1024.0f;
        row0[3] = ldexpf(1.0f, sc);
        row1[3] = 0.0f;
    }
    UNI(uIndMtx, indmtx, im, glUniform4fv(sp->uIndMtx, 6, im));

    float efb[2] = {float(s_efbW), float(EFB_H)};
    UNI(uEfb, efb, efb, glUniform2fv(sp->uEfb, 1, efb));
    float proj[8] = {xff(XFR_PROJ), xff(XFR_PROJ + 1), xff(XFR_PROJ + 2), xff(XFR_PROJ + 3),
                     xff(XFR_PROJ + 4), xff(XFR_PROJ + 5), float(g.xfReg[XFR_PROJ + 6] & 1), 0.0f};
    UNI(uProj, proj, proj, glUniform4fv(sp->uProj, 2, proj));
    float vp[8] = {xff(XFR_VIEWPORT), xff(XFR_VIEWPORT + 1), xff(XFR_VIEWPORT + 2), 0.0f,
                   xff(XFR_VIEWPORT + 3), xff(XFR_VIEWPORT + 4), xff(XFR_VIEWPORT + 5), 0.0f};
    if (s_efbW != EFB_W) {  // the draw's widescreen mapping (centre and half-width, clip x scale)
        vp[4] = (vp[4] - 342.0f) * s_xmap.a + s_xmap.b + 342.0f;
        vp[0] *= s_xmap.a * s_xmap.clip;
    }
    UNI(uViewport, vp, vp, glUniform4fv(sp->uViewport, 2, vp));
    float ch[16];
    const int regs[4] = {XFR_AMB0, XFR_AMB0 + 1, XFR_MAT0, XFR_MAT0 + 1};
    for (int i = 0; i < 4; i++) {
        uint32_t v = g.xfReg[regs[i]];
        ch[4 * i + 0] = float((v >> 24) & 255) / 255.0f;
        ch[4 * i + 1] = float((v >> 16) & 255) / 255.0f;
        ch[4 * i + 2] = float((v >> 8) & 255) / 255.0f;
        ch[4 * i + 3] = float(v & 255) / 255.0f;
    }
    UNI(uAmbMat, ambmat, ch, glUniform4fv(sp->uAmbMat, 4, ch));
#undef UNI
    uc.init = true;
}

static uint32_t s_syncReads = 0;  // reads that made the CPU wait for the GPU
static uint64_t s_flushes = 0;    // flushBatch calls that drew
static void statsFrame() {
    static int every = -1;
    static uint32_t frames = 0, draws = 0, verts = 0, compiles0 = 0, uploads0 = 0, sync0 = 0;
    static double t0 = 0, gx0 = 0, tex0 = 0, draw0 = 0, copy0 = 0, peek0 = 0, wait0 = 0;
    if (every < 0) {
        const char* e = getenv("SMS_GX_STATS");
        every = e ? atoi(e) : 0;
        t0 = nowSeconds();
    }
    if (every <= 0) return;
    frames++;
    draws += s_stats.draws;
    verts += s_stats.vertices;
    if (frames < uint32_t(every)) return;
    double t = nowSeconds();
    logmsg("stats: %u frames, %.1f draws/frame, %.0f vertices/frame, %u shader compiles, %u texture uploads, "
           "%.1f ms/frame total, %.1f ms/frame in sms_gx (textures %.1f, batches %.1f, copies %.1f, peeks %.1f, "
           "GPU waits %.1f), %.1f synchronous GPU reads/frame",
           frames, double(draws) / frames, double(verts) / frames, g_statShaderCompiles - compiles0,
           g_statTexUploads - uploads0, (t - t0) * 1000.0 / frames, (s_gxSeconds + g_decodeSeconds - gx0) * 1000.0 / frames,
           (s_texSeconds - tex0) * 1000.0 / frames, (g_flushSeconds - s_texSeconds - draw0) * 1000.0 / frames,
           (s_copySeconds - copy0) * 1000.0 / frames, (s_peekSeconds - peek0) * 1000.0 / frames,
           (s_waitSeconds - wait0) * 1000.0 / frames, double(s_syncReads - sync0) / frames);
    static uint64_t gl0 = 0, hash0 = 0, inv0 = 0, flush0 = 0;
    static double dec0 = 0, idle0 = 0;
    logmsg("stats: %.0f GL calls/frame, %.0f batch flushes/frame, %.0f KiB texture data hashed/frame, "
           "%.1f texture invalidations/frame, %.2f ms/frame decoding vertices, %.1f ms/frame idle",
           double(gl::g_statGlCalls - gl0) / frames, double(s_flushes - flush0) / frames,
           double(g_statTexHashBytes - hash0) / 1024.0 / frames, double(g_statTexInvalidates - inv0) / frames,
           (g_decodeSeconds - dec0) * 1000.0 / frames, ((s_idleClock ? s_idleClock() : 0) - idle0) * 1000.0 / frames);
    gl::logTopCalls(frames);
    dec0 = g_decodeSeconds;
    idle0 = s_idleClock ? s_idleClock() : 0;
    gl0 = gl::g_statGlCalls;
    hash0 = g_statTexHashBytes;
    inv0 = g_statTexInvalidates;
    flush0 = s_flushes;
    sync0 = s_syncReads;
    tex0 = s_texSeconds;
    draw0 = g_flushSeconds - s_texSeconds;
    wait0 = s_waitSeconds;
    copy0 = s_copySeconds;
    peek0 = s_peekSeconds;
    frames = draws = verts = 0;
    compiles0 = g_statShaderCompiles;
    uploads0 = g_statTexUploads;
    t0 = t;
    gx0 = s_gxSeconds + g_decodeSeconds;
}

// Asynchronous GPU reads. Every synchronous read (glReadPixels into client
// memory, a query result) makes the CPU wait for all queued GPU work, which on
// a real GPU serialises the two. Reads the game makes every frame are answered
// from the same read one frame earlier instead (as Dolphin does), which the
// game cannot tell apart; SMS_GX_SYNC_READS=1 goes back to synchronous reads.
static uint32_t s_frameNo = 0;   // display copies so far
static uint32_t s_drawGen = 0;   // bumped by anything that changes the EFB
static bool asyncReads() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SMS_GX_SYNC_READS");
        on = !(e && e[0] == '1');
    }
    return on != 0;
}

// Pixel metrics (GXClearPixMetric/GXReadPixMetric). Delfino's pollution
// counters draw each goop layer with an alpha test and read how many pixels
// reached the colour unit; the game subtracts 4 per polygon of what it drew,
// so the count is the samples that passed plus 4 per triangle. Copy passes
// in between are left out: they end the running query, which is summed at
// the read. Each clear/read pair of a frame is a slot; a slot that drew the
// same number of triangles one frame earlier, in one query, is answered from
// that frame's query, otherwise the queries are waited for.
enum { kMetricSlots = 32 };
static GLuint s_mq[2][kMetricSlots];
static uint32_t s_mqTris[2][kMetricSlots], s_mqFrame[2][kMetricSlots];
static bool s_mqUsed[2][kMetricSlots];
static int s_mqSlot = -1;
static uint32_t s_mqSlotFrame = ~0u;
static bool s_pixActive = false, s_pixTaint = false;
static uint32_t s_pixTris = 0;
static uint64_t s_pixSamples = 0;  // counted before the pending queries
static GLuint s_pixCur = 0;
static bool s_pixCurPooled = false;
static std::vector<GLuint> s_queryPool;
static std::vector<std::pair<GLuint, bool>> s_pixPending;  // ended queries of this count, pooled?

static GLuint poolQuery() {
    if (s_queryPool.empty()) {
        GLuint q;
        glGenQueries(1, &q);
        return q;
    }
    GLuint q = s_queryPool.back();
    s_queryPool.pop_back();
    return q;
}
static void pixReleasePending() {
    for (auto& q : s_pixPending)
        if (q.second) s_queryPool.push_back(q.first);
    s_pixPending.clear();
}
static uint64_t queryResult(GLuint q) {
    GxTimer tw(&s_waitSeconds);
    GLuint n = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &n);
    return n;
}

void pixMetricClear() {
    flushBatch();
    if (!s_ready) return;
    if (s_pixActive) {
        glEndQuery(GL_SAMPLES_PASSED);
        if (s_pixCurPooled) s_queryPool.push_back(s_pixCur);
    }
    pixReleasePending();
    if (s_mqSlotFrame != s_frameNo) {
        s_mqSlotFrame = s_frameNo;
        s_mqSlot = -1;
    }
    s_mqSlot++;
    int cur = s_frameNo & 1;
    if (s_mqSlot < kMetricSlots) {
        if (!s_mq[cur][s_mqSlot]) glGenQueries(1, &s_mq[cur][s_mqSlot]);
        s_pixCur = s_mq[cur][s_mqSlot];
        s_pixCurPooled = false;
    } else {
        s_pixCur = poolQuery();
        s_pixCurPooled = true;
    }
    s_pixSamples = 0;
    s_pixTris = 0;
    s_pixTaint = !asyncReads() || s_mqSlot >= kMetricSlots;
    glBeginQuery(GL_SAMPLES_PASSED, s_pixCur);
    s_pixActive = true;
}

uint32_t pixMetricRead() {
    flushBatch();
    if (!s_ready || !s_pixActive) return 0;
    glEndQuery(GL_SAMPLES_PASSED);
    uint64_t samples = 0;
    int cur = s_frameNo & 1, slot = s_mqSlot;
    bool fromPrev = false;
    if (!s_pixTaint) {
        s_mqUsed[cur][slot] = true;
        s_mqTris[cur][slot] = s_pixTris;
        s_mqFrame[cur][slot] = s_frameNo;
        int prev = cur ^ 1;
        if (s_mqUsed[prev][slot] && s_mqFrame[prev][slot] + 1 == s_frameNo && s_mqTris[prev][slot] == s_pixTris) {
            samples = queryResult(s_mq[prev][slot]);
            fromPrev = true;
        }
    }
    if (!fromPrev) {
        s_syncReads++;
        samples = s_pixSamples + queryResult(s_pixCur);
        for (auto& q : s_pixPending) samples += queryResult(q.first);
    }
    pixReleasePending();
    if (s_pixCurPooled) s_queryPool.push_back(s_pixCur);
    // reading does not reset the hardware counter: keep counting (a second
    // read before the next clear waits for its queries)
    s_pixSamples = samples;
    s_pixTaint = true;
    s_pixCur = poolQuery();
    s_pixCurPooled = true;
    glBeginQuery(GL_SAMPLES_PASSED, s_pixCur);
    uint64_t S2 = uint64_t(s_scale) * uint64_t(s_scale);
    uint64_t v = samples / S2 + uint64_t(s_pixTris) * 4;
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(v);
}

static void pixMetricPause() {
    if (s_pixActive) {
        glEndQuery(GL_SAMPLES_PASSED);
        s_pixPending.emplace_back(s_pixCur, s_pixCurPooled);
        s_pixTaint = true;  // this count now spans several queries
    }
}
static void pixMetricResume() {
    if (s_pixActive) {
        s_pixCur = poolQuery();
        s_pixCurPooled = true;
        glBeginQuery(GL_SAMPLES_PASSED, s_pixCur);
    }
}

// A batch's horizontal extent on screen (game coordinates), from its
// positions, position matrices and the projection and viewport.
static void screenExtent(float sx, float cx, float* outLo, float* outHi) {
    const VtxFmtLayout& l = vtxFmtLayout(s_bfmt);
    float p0 = xff(XFR_PROJ), p1 = xff(XFR_PROJ + 1);
    bool ortho = g.xfReg[XFR_PROJ + 6] & 1;
    uint32_t defIdx = g.xfReg[XFR_MATIDX_A] & 63;
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t v = 0; v < s_bcount; v++) {
        const uint8_t* vx = s_bdata.data + size_t(v) * s_bstride;
        float pos[3];
        memcpy(pos, vx, 12);
        uint32_t idx = (s_bfmt & VF_MTX) ? (vx[l.mtx] & 63) : defIdx;
        float m[12];
        memcpy(m, &g.xfMem[idx * 4], 48);
        float x = m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3];
        float X;
        if (ortho) {
            X = cx + sx * (p0 * x + p1);
        } else {
            float z = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11];
            if (z > -1e-6f) continue;  // behind the eye
            X = cx + sx * (p0 * x + p1 * z) / -z;
        }
        lo = std::min(lo, X);
        hi = std::max(hi, X);
    }
    *outLo = lo;
    *outHi = hi;
}

// How the current batch's game coordinates map into the widened EFB.
static XMap drawXMap() {
    XMap m;
    if (s_efbW == EFB_W) return m;
    if (s_stretch2D && (g.xfReg[XFR_PROJ + 6] & 1)) {  // a fade or wipe: all of it across the frame
        m.a = s_wide;
        return m;
    }
    float sx = xff(XFR_VIEWPORT), cx = xff(XFR_VIEWPORT + 3) - 342.0f;
    bool fullWidth = fabsf(fabsf(sx) * 2.0f - float(EFB_W)) < 2.0f && fabsf(cx - float(EFB_W) / 2) < 2.0f;
    if (!fullWidth) {
        m.b = centredOffset(cx);
        return m;
    }
    if ((g.xfReg[XFR_PROJ + 6] & 1) == 0) {  // perspective
        if (s_hud) {  // a 3D part of the HUD (the water tank): moved like the rest
            float lo, hi;
            screenExtent(sx, cx, &lo, &hi);
            if (lo <= hi) m.b = centredOffset((lo + hi) / 2);
            return m;
        }
        m.a = s_wide;
        float p00 = xff(XFR_PROJ), p11 = xff(XFR_PROJ + 2);
        float aspect = p00 != 0.0f ? fabsf(p11 / p00) : 0.0f;
        if (aspect < kCamAspect * sqrtf(s_wide)) m.clip = 1.0f / s_wide;  // not widened by the game
        return m;
    }
    // 2D across the whole width: artwork (menus, the map, movies: colour
    // from an image) stays centred; fades, masks and passes over the frame
    // (untextured, a tiny utility texture, alpha only, or drawing screen
    // copies back) are stretched
    bool artwork = false;
    bool colour = (g.bp[BP_CMODE0] >> 3) & 1;  // colour update
    uint32_t gen = g.bp[BP_GENMODE];
    uint32_t nst = ((gen >> 10) & 15) + 1;
    for (uint32_t st = 0; colour && st < nst && !artwork; st++) {
        uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0x3FF;
        if (!((ord >> 6) & 1)) continue;
        int map = int(ord & 7);
        const uint8_t* ptr = g.texImage[map];
        uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
        uint32_t tw = (img0 & 0x3FF) + 1, th = ((img0 >> 10) & 0x3FF) + 1;
        int cw, ch;
        if (ptr && tw >= 32 && th >= 32 && !efbCopyLookup(ptr, &cw, &ch)) artwork = true;
    }
    float lo, hi;
    screenExtent(sx, cx, &lo, &hi);
    bool spans = lo <= 2.0f && hi >= float(EFB_W) - 2.0f;
    if (spans && !artwork) m.a = s_wide;
    else m.b = spans ? float(s_ox) : centredOffset((lo + hi) / 2);
    return m;
}

void flushBatch() {
    if (!s_bidx.size || !s_ready) {
        s_bidx.size = s_bdata.size = 0;
        s_bcount = 0;
        return;
    }
    GxTimer timer;
    GxTimer tf(&g_flushSeconds);
    s_xmap = drawXMap();
    const ShaderProgram* sp = shaderForCurrentState();
    glcUseProgram(sp->prog);
    applyGlState();

    // textures: every map referenced by an enabled TEV stage or an indirect stage
    float texW[8] = {0}, texH[8] = {0};
    uint32_t used = 0;
    uint32_t gen = g.bp[BP_GENMODE];
    uint32_t nst = ((gen >> 10) & 15) + 1, nind = (gen >> 16) & 7;
    for (uint32_t s = 0; s < nst; s++) {
        uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        if ((ord >> 6) & 1) used |= 1u << (ord & 7);
    }
    for (uint32_t i = 0; i < nind && i < 4; i++) used |= 1u << ((g.bp[BP_RAS1_IREF] >> (6 * i)) & 7);
    for (int m = 0; m < 8; m++) {
        if (!(used & (1u << m))) continue;
        GxTimer tt(&s_texSeconds);
        bindTextureMap(m, &texW[m], &texH[m]);
    }

    uploadUniforms(sp, texW, texH);

    glcBindVertexArray(vaoForFormat(s_bfmt));
    uint32_t matA = g.xfReg[XFR_MATIDX_A], matB = g.xfReg[XFR_MATIDX_B];
    uint8_t defMtx[12] = {
        uint8_t(matA & 63), uint8_t((matA >> 6) & 63), uint8_t((matA >> 12) & 63), uint8_t((matA >> 18) & 63),
        uint8_t((matA >> 24) & 63), uint8_t(matB & 63), uint8_t((matB >> 6) & 63), uint8_t((matB >> 12) & 63),
        uint8_t((matB >> 18) & 63), 0, 0, 0,
    };
    if (!(s_bfmt & VF_MTX)) {  // the batch's default matrix indices, as a constant attribute
        static uint32_t cur[3] = {~0u, ~0u, ~0u};
        uint32_t m[3];
        memcpy(m, defMtx, 12);  // the shader unpacks the bytes little-endian
        if (memcmp(m, cur, 12) != 0) {
            memcpy(cur, m, 12);
            glVertexAttribI4ui(14, m[0], m[1], m[2], 0);
        }
    }
    // this batch's XF block (when it changed), vertices and indices, in one map
    static uint32_t xfBlock[736];
    static size_t s_xfOffset = 0;
    StreamPiece pc[3];
    int np = 0;
    size_t worst = sizeof(xfBlock) + size_t(s_uboAlign) + s_bdata.size + s_bstride + s_bidx.size * 4 + 4;
    // Starting a new lap of the stream overwrites older data, possibly the
    // block still bound: write it again with this batch.
    if (s_stream.wouldWrap(worst)) s_xfDirty = true;
    bool xfWritten = s_xfDirty;
    if (s_xfDirty) {
        memcpy(xfBlock, &g.xfMem[0], 256 * 4);
        memcpy(xfBlock + 256, &g.xfMem[0x400], 96 * 4);
        memcpy(xfBlock + 352, &g.xfMem[0x500], 256 * 4);
        memcpy(xfBlock + 608, &g.xfMem[0x600], 128 * 4);
        pc[np++] = {xfBlock, sizeof(xfBlock), size_t(s_uboAlign), 0};
        s_xfDirty = false;
    }
    pc[np++] = {s_bdata.data, s_bdata.size, s_bstride, 0};
    pc[np++] = {s_bidx.data, s_bidx.size * 4, 4, 0};
    s_stream.appendAll(pc, np);
    if (xfWritten) s_xfOffset = pc[0].at;
    if (xfWritten || g_glc.uniformBuffer == ~0u) {
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, s_stream.buf, GLintptr(s_xfOffset), sizeof(xfBlock));
        g_glc.uniformBuffer = s_stream.buf;  // the range bind also sets the generic binding
    }
    size_t vOff = pc[np - 2].at, iOff = pc[np - 1].at;
    GLenum mode = s_bclass == PRIM_TRIS ? GL_TRIANGLES : s_bclass == PRIM_LINES ? GL_LINES : GL_POINTS;
    bool earlyFallback = !shaderHasEarlyFragmentTests() && earlyZWritesRejected();
    if (earlyFallback) {
        const ShaderProgram* depthSp = shaderForCurrentState(true);
        glcUseProgram(depthSp->prog);
        uploadUniforms(depthSp, texW, texH);
        GLboolean masks[4];
        memcpy(masks, g_glc.cmask, sizeof masks);
        size_t step = s_bclass == PRIM_TRIS ? 3 : s_bclass == PRIM_LINES ? 2 : 1;
        // Preserve primitive order and the original comparison (including LESS
        // and NOTEQUAL): colour tests the old depth, then an unconditional
        // fragment shader writes depth even where the alpha test discarded.
        // A batch-wide prepass followed by EQUAL would change overlapping draws.
        // Pixel metrics count the colour pass, whose pixels passed the alpha
        // test, and not the depth pass.
        for (size_t i = 0; i < s_bidx.size; i += step) {
            const void* indices = reinterpret_cast<const void*>(iOff + i * 4);
            glcUseProgram(sp->prog);
            glColorMask(masks[0], masks[1], masks[2], masks[3]);
            glDepthMask(GL_FALSE);
            glDrawElementsBaseVertex(mode, GLsizei(step), GL_UNSIGNED_INT, indices, GLint(vOff / s_bstride));
            pixMetricPause();
            glcUseProgram(depthSp->prog);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            glDrawElementsBaseVertex(mode, GLsizei(step), GL_UNSIGNED_INT, indices, GLint(vOff / s_bstride));
            pixMetricResume();
        }
        glColorMask(masks[0], masks[1], masks[2], masks[3]);
        glcUseProgram(sp->prog);
    } else {
        glDrawElementsBaseVertex(mode, GLsizei(s_bidx.size), GL_UNSIGNED_INT, reinterpret_cast<const void*>(iOff),
                                 GLint(vOff / s_bstride));
    }
    s_stats.draws++;
    s_flushes++;
    s_drawGen++;
    if (s_pixActive && s_bclass == PRIM_TRIS) s_pixTris += uint32_t(s_bidx.size / 3);
    if (traceFile()) {
        static std::vector<HostVertex> hv;
        hv.resize(s_bcount);
        for (uint32_t i = 0; i < s_bcount; i++) unpackVertex(s_bfmt, s_bdata.data + size_t(i) * s_bstride, defMtx, hv[i]);
        traceDraw(int(s_bclass), s_bcount, uint32_t(s_bidx.size), hv.data(), sp->id);
    }
    s_bidx.size = s_bdata.size = 0;
    s_bcount = 0;
    if (traceFile()) {
        traceProbe();
        glcInvalidate();
    }
}

// ------------------------------------------------------------------ EFB copies
// The framebuffer whose attachments hold the EFB's current pixels, and whose
// colour and depth are s_efbColor/s_efbDepth: with MSAA, the multisampled EFB
// is resolved into it first. Leaves the scissor test off.
static GLuint efbReadFbo() {
    if (!s_msaa) return s_efbFbo;
    const int W = s_efbW * s_scale, H = EFB_H * s_scale;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efbFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_efbResolveFbo);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return s_efbResolveFbo;
}

static void clearRect(int x, int y, int w, int h) {
    uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB], z = g.bp[BP_CLEAR_Z];
    glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * s_scale, y * s_scale, w * s_scale, h * s_scale);
    bool hasAlpha = (g.bp[BP_PE_CONTROL] & 7) == 1;
    // Copy clears obey the PE write enables, just like ordinary EFB writes.
    // A copy clear in RGB8 must not overwrite the backing alpha used later by
    // SMS's RGBA6 shadow volumes, nor may a disabled alpha/depth write do so.
    bool colour = (g.bp[BP_CMODE0] >> 3) & 1;
    bool alpha = hasAlpha && ((g.bp[BP_CMODE0] >> 4) & 1);
    glColorMask(colour, colour, colour, alpha);
    glDepthMask((g.bp[BP_ZMODE] >> 4) & 1);
    glClearColor(float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f, float(gb & 255) / 255.0f,
                 hasAlpha ? float((ar >> 8) & 255) / 255.0f : 1.0f);
    glClearDepth(double(z & 0xFFFFFF) / 16777215.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// GXCopyTex stores into main memory on the hardware, and the game reads some
// copies back on the CPU: Delfino's goop is cleaned by drawing into the EFB
// and copying into the pollution texture, whose bytes TPollutionLayer::
// isPolluted then reads. So every texture copy is also read back and stored
// in RAM in its GX layout (the GL copy stays as the fast path for sampling).
// SMS_GX_COPY_WRITEBACK=0 turns this off.
static bool copyWriteBackEnabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SMS_GX_COPY_WRITEBACK");
        on = !(e && e[0] == '0');
    }
    return on != 0;
}

// px: the copy as read back (ow x oh); stored as the tw x th texels the
// game asked for (sampled nearest when the EFB scale or widescreen made the
// copy larger).
static void encodeAndStore(const uint8_t* px, const void* dest, int ow, int oh, int tw, int th, uint32_t layout) {
    static std::vector<uint8_t> texels, enc;
    const uint8_t* src = px;
    if (ow != tw || oh != th) {
        texels.resize(size_t(tw) * th * 4);
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                memcpy(&texels[(size_t(y) * tw + x) * 4],
                       &px[(size_t(y) * oh / th * ow + size_t(x) * ow / tw) * 4], 4);
        src = texels.data();
    }
    enc.resize(texLevelBytes(layout, tw, th));
    uint32_t n = encodeTexture(src, layout, tw, th, enc.data());
    static int logCopies = -1;
    if (logCopies < 0) logCopies = getenv("SMS_GX_COPY_LOG") ? atoi(getenv("SMS_GX_COPY_LOG")) : 0;
    if (logCopies > 0) {
        uint32_t changed = 0;
        for (uint32_t i = 0; i < n; i++) changed += enc[i] != static_cast<const uint8_t*>(dest)[i];
        logmsg("copy write-back %p layout %u %dx%d: %u bytes, %u changed", dest, layout, tw, th, n, changed);
        logCopies--;
    }
    memcpy(const_cast<void*>(dest), enc.data(), n);
    textureInvalidateRange(dest, n);
}

// A texture copy's write-back is read into a pixel buffer and stored one
// frame later, when the GPU has long finished it (or earlier, when the next
// copy to the same place comes). It is dropped when a newer copy of the same
// frame supersedes it or when the destination's bytes
// changed in the meantime (the CPU wrote them, or a stage load reused the
// memory). A cache flush of the range alone does not drop it: games flush
// copy destinations without writing them.
struct PendingWriteBack {
    const void* dest;
    uint32_t bytes;
    uint64_t hash;
    GLuint pbo;
    GLsync fence;
    int ow, oh, tw, th;
    uint32_t layout, frame;
    bool cancelled;
};
static std::vector<PendingWriteBack> s_writeBacks;
static std::vector<GLuint> s_freePbos;

static void resolveWriteBack(PendingWriteBack& w) {
    if (!w.cancelled && hashBytes(w.dest, w.bytes) == w.hash) {
        const void* px;
        {
            GxTimer tw(&s_waitSeconds);
            glClientWaitSync(w.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, w.pbo);
            px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(w.ow) * w.oh * 4, GL_MAP_READ_BIT);
        }
        if (px) encodeAndStore(static_cast<const uint8_t*>(px), w.dest, w.ow, w.oh, w.tw, w.th, w.layout);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glDeleteSync(w.fence);
    s_freePbos.push_back(w.pbo);
}

// Stores the write-backs issued before this frame (all of them with `all`).
static void resolveWriteBacks(bool all) {
    size_t keep = 0;
    for (size_t i = 0; i < s_writeBacks.size(); i++) {
        PendingWriteBack& w = s_writeBacks[i];
        if (!all && !w.cancelled && w.frame >= s_frameNo) s_writeBacks[keep++] = w;
        else resolveWriteBack(w);
    }
    s_writeBacks.resize(keep);
}

// tw x th: the copy's size in texels as the game sees it.
static void writeBackCopy(const void* dest, GLuint tex, int ow, int oh, int tw, int th, uint32_t layout) {
    if (!dest || !copyWriteBackEnabled()) return;
    if (tw < 1 || th < 1) return;
    uint32_t bytes = texLevelBytes(layout, tw, th);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_tmpFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    efbCopySetBytes(dest, bytes);
    if (!asyncReads()) {
        s_syncReads++;
        static std::vector<uint8_t> px;
        px.resize(size_t(ow) * oh * 4);
        glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        encodeAndStore(px.data(), dest, ow, oh, tw, th, layout);
        return;
    }
    // A copy to the same place supersedes one of this frame; one from an
    // earlier frame (most games copy to the same buffer every frame) is
    // stored first, as the CPU may have read it in between.
    size_t keep = 0;
    for (size_t i = 0; i < s_writeBacks.size(); i++) {
        PendingWriteBack& w = s_writeBacks[i];
        if (w.dest != dest) s_writeBacks[keep++] = w;
        else if (w.frame < s_frameNo) resolveWriteBack(w);
        else {
            w.cancelled = true;
            resolveWriteBack(w);
        }
    }
    s_writeBacks.resize(keep);
    GLuint pbo;
    if (!s_freePbos.empty()) {
        pbo = s_freePbos.back();
        s_freePbos.pop_back();
    } else {
        glGenBuffers(1, &pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(ow) * oh * 4, nullptr, GL_STREAM_READ);
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    PendingWriteBack w{dest, bytes, hashBytes(dest, bytes), pbo, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0),
                       ow, oh, tw, th, layout, s_frameNo, false};
    s_writeBacks.push_back(w);
    (void)tex;
}

static void copyEfb(uint32_t ctrl) {
    GxTimer timer;
    GxTimer tc(&s_copySeconds);
    uint32_t src = g.bp[BP_COPY_SRC_TL], size = g.bp[BP_COPY_SRC_WH];
    int x = int(src & 0x3FF), y = int((src >> 10) & 0x3FF);
    int w = int(size & 0x3FF) + 1, h = int((size >> 10) & 0x3FF) + 1;
    const void* dest = g.copyDest;
    bool disp = (ctrl >> 14) & 1;
    bool clear = (ctrl >> 11) & 1;
    if (traceFile()) traceCopy(disp, x, y, w, h, dest, ctrl);
    int S = s_scale;
    // widescreen: a full-width copy takes the whole EFB, others the centred 4:3 part
    int gw = w;
    if (s_efbW != EFB_W) {
        if (x <= 0 && x + w >= EFB_W) {
            x = 0;
            w = s_efbW;
        } else if (s_stretch2D) {  // a fader's pieces of the frame: stretched like its drawing
            int r = int(float(x + w) * s_wide + 0.5f);
            x = int(float(x) * s_wide + 0.5f);
            w = r - x;
        } else {
            x += s_ox;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE0 + 1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (disp) {
        Xfb& xfb = s_xfbs[dest];
        if (!xfb.tex || xfb.w != w * S || xfb.h != h * S) {
            if (!xfb.tex) glGenTextures(1, &xfb.tex);
            glBindTexture(GL_TEXTURE_2D, xfb.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * S, h * S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            xfb.w = w * S;
            xfb.h = h * S;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, efbReadFbo());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_tmpFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xfb.tex, 0);
        glBlitFramebuffer(x * S, y * S, (x + w) * S, (y + h) * S, 0, 0, w * S, h * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        s_lastXfb = dest;
        s_stats.efbCopies++;
    } else {
        uint32_t fmt = ((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7);
        bool intensity = ((ctrl >> 15) & 3) == 3;
        bool zcopy = (g.bp[BP_PE_CONTROL] & 7) == 3;
        bool half = (ctrl >> 9) & 1;
        int ow = (half ? w / 2 : w) * S, oh = (half ? h / 2 : h) * S;
        int tw = half ? gw / 2 : gw, th = half ? h / 2 : h;
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        int cw = 0, chh = 0;
        GLuint tex = efbCopyLookup(dest, &cw, &chh);
        if (!tex || cw != ow || chh != oh) {
            if (!tex) glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        uint32_t mode = fmt | (intensity ? 16u : 0u) | (zcopy ? 32u : 0u);
        efbCopyRegister(dest, tex, ow, oh, mode);
        efbReadFbo();  // s_efbColor/s_efbDepth, sampled below, hold the current EFB
        glBindFramebuffer(GL_FRAMEBUFFER, s_tmpFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glViewport(0, 0, ow, oh);
        glUseProgram(s_copyProg);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_efbDepth);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_efbColor);
        float rect[4] = {float(x) / float(s_efbW), float(y) / EFB_H, float(w) / float(s_efbW), float(h) / EFB_H};
        glUniform4fv(s_copyURect, 1, rect);
        glUniform1i(s_copyUMode, GLint(mode));
        glUniform1i(s_copyUAlphaOne, (g.bp[BP_PE_CONTROL] & 7) != 1);
        glBindVertexArray(s_copyVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(s_vao);
        s_stats.efbCopies++;
        writeBackCopy(dest, tex, ow, oh, tw, th, copyLayout(fmt, zcopy));
    }
    if (clear) clearRect(x, y, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
    if (clear) s_drawGen++;
    if (disp) {
        resolveWriteBacks(false);
        hiresEndFrame();
        s_frameNo++;
        traceFrameAdvance();
        statsFrame();
        s_lastFrameStats = s_stats;
        s_stats.draws = s_stats.vertices = s_stats.efbCopies = 0;
    }
}

void executeCopy(uint32_t ctrl) {
    if (!s_ready) return;
    flushBatch();
    glcInvalidate();  // copies, clears and write-backs set GL state directly
    pixMetricPause();
    struct Resume { ~Resume() { pixMetricResume(); } } resume;
    const void* dest = g.copyDest;
    bool disp = (ctrl >> 14) & 1;
    copyEfb(ctrl);
    // presenting (the hook) is timed apart from sms_gx: GXPC_GetTimes
    if (disp && g_displayCopyHook) g_displayCopyHook(dest);
}

}  // namespace gx

using namespace gx;

extern "C" {

int GXPC_Init(GXPCGetProcFn getProc, int efbScale) {
    if (!gl::load(getProc)) return 0;
    resetState();
    rendererInit(efbScale);
    hiresEnabled();  // immutable index/format support published before loader threads run
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void GXPC_PrefetchResource(const void* data, uint32_t size, const char* name) {
    hiresPrefetchResource(data, size, name);
}

void GXPC_PreloadTextures(void) {
    if (!s_ready) return;
    flushBatch();
    glcInvalidate();
    hiresPreload();
}

void GXPC_Shutdown(void) {
    if (!s_ready) return;
    textureShutdown();
    shaderShutdown();
    if (s_overlayTex) glDeleteTextures(1, &s_overlayTex);
    if (s_overlayProg) glDeleteProgram(s_overlayProg);
    s_overlayTex = s_overlayProg = 0;
    s_overlayW = s_overlayH = 0;
    invalidateOverlay();
    for (auto& kv : s_xfbs) glDeleteTextures(1, &kv.second.tex);
    s_xfbs.clear();
    s_ready = false;
}

void GXPC_InvalidateRange(const void* p, uint32_t size) {
    if (copyWriteBackEnabled()) textureCpuWrote(p, size);
    else textureInvalidateRange(p, size);
}

}  // extern "C"

namespace gx {
// GXPeekARGB/GXPeekZ: the sun's lens-flare test peeks 17 depths a frame and
// Mario's occlusion test one colour. Peeks with no drawing in between form a
// group; the first peek of a group starts an asynchronous read of the whole
// buffer, and the group is answered from the same group's read one frame
// earlier (synchronously the first time a group appears).
enum { kPeekGroups = 8 };
struct PeekSnap {
    GLuint pbo = 0;
    GLsync fence = 0;
    uint32_t frame = ~0u;
    const uint8_t* map = nullptr;
};
static PeekSnap s_peek[2][kPeekGroups][2];  // [frame parity][group][colour, depth]
static uint32_t s_peekDrawGen = ~0u, s_peekFrame = ~0u, s_peekIssued = 0;
static int s_peekGroup = -1;

static uint32_t peekSync(int x, int y, bool depth) {
    glcInvalidate();
    GxTimer tw(&s_waitSeconds);
    s_syncReads++;
    uint32_t v = 0;
    uint8_t px[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, efbReadFbo());
    if (depth) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x * s_scale, y * s_scale, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &v);
        return v;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x * s_scale, y * s_scale, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return uint32_t(px[0]) | uint32_t(px[1]) << 8 | uint32_t(px[2]) << 16 | uint32_t(px[3]) << 24;
}

// A frame's peek snapshot is read at 1x: with a larger EFB scale the EFB is
// first blitted (nearest) into this buffer, so a read stays 1.3 MiB.
static GLuint s_peekFbo, s_peekColor, s_peekDepth;

static GLuint peekSource() {
    if (s_scale == 1) return efbReadFbo();
    if (!s_peekFbo) {
        glGenRenderbuffers(1, &s_peekColor);
        glBindRenderbuffer(GL_RENDERBUFFER, s_peekColor);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, s_efbW, EFB_H);
        glGenRenderbuffers(1, &s_peekDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, s_peekDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, s_efbW, EFB_H);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &s_peekFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_peekFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_peekColor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_peekDepth);
    }
    glDisable(GL_SCISSOR_TEST);
    const GLuint src = efbReadFbo();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_peekFbo);
    glBlitFramebuffer(0, 0, s_efbW * s_scale, EFB_H * s_scale, 0, 0, s_efbW, EFB_H,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return s_peekFbo;
}

// Returns the raw texel: RGBA bytes packed little-endian, or the 32-bit depth.
static uint32_t peekRaw(int x, int y, bool depth) {
    if (s_efbW != EFB_W && x >= 0) x = int((float(x) + 0.5f) * s_wide);  // the game camera's coordinates: stretched
    flushBatch();
    glcInvalidate();
    GxTimer tp(&s_peekSeconds);
    if (!asyncReads() || x < 0 || y < 0 || x >= s_efbW || y >= EFB_H) return peekSync(x, y, depth);
    if (s_peekDrawGen != s_drawGen || s_peekFrame != s_frameNo) {
        if (s_peekFrame != s_frameNo) s_peekGroup = -1;
        s_peekGroup++;
        s_peekDrawGen = s_drawGen;
        s_peekFrame = s_frameNo;
        s_peekIssued = 0;
    }
    if (s_peekGroup >= kPeekGroups) return peekSync(x, y, depth);
    const GLsizeiptr bytes = GLsizeiptr(s_efbW) * EFB_H * 4;
    int cur = s_frameNo & 1, t = depth ? 1 : 0;
    if (!(s_peekIssued & (1u << t))) {
        s_peekIssued |= 1u << t;
        PeekSnap& now = s_peek[cur][s_peekGroup][t];
        if (!now.pbo) glGenBuffers(1, &now.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, now.pbo);
        if (now.map) {
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            now.map = nullptr;
        }
        if (now.fence) glDeleteSync(now.fence);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, peekSource());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        if (depth) glReadPixels(0, 0, s_efbW, EFB_H, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        else glReadPixels(0, 0, s_efbW, EFB_H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
        now.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        now.frame = s_frameNo;
    }
    PeekSnap& prev = s_peek[cur ^ 1][s_peekGroup][t];
    if (prev.pbo && prev.frame + 1 == s_frameNo) {
        if (!prev.map) {
            GxTimer tw(&s_waitSeconds);
            glClientWaitSync(prev.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, prev.pbo);
            prev.map = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        if (prev.map) {
            uint32_t v;
            memcpy(&v, prev.map + (size_t(y) * s_efbW + x) * 4, 4);
            return v;
        }
    }
    return peekSync(x, y, depth);
}

uint32_t peekColor(int x, int y) {
    uint32_t p = peekRaw(x, y, false);
    uint32_t c = (p >> 24) << 24 | (p & 0xFF) << 16 | ((p >> 8) & 0xFF) << 8 | ((p >> 16) & 0xFF);
    if (FILE* f = traceFile()) fprintf(f, "  GXPeekARGB(%d,%d) = %08X\n", x, y, c);
    return c;
}
uint32_t peekZ(int x, int y) {
    uint32_t v = peekRaw(x, y, true);
    if (FILE* f = traceFile()) fprintf(f, "  GXPeekZ(%d,%d) = %06X\n", x, y, v >> 8);
    return v >> 8;
}
}  // namespace gx

extern "C" {

int GXPC_PresentXFB(const void* xfb, int winW, int winH) {
    flushBatch();
    glcInvalidate();
    if (!xfb) xfb = s_lastXfb;
    auto it = s_xfbs.find(xfb);
    if (it == s_xfbs.end()) return 0;
    const Xfb& x = it->second;
    // letterbox to 4:3, or to the widened aspect
    float aspect = 4.0f / 3.0f * float(s_efbW) / float(EFB_W);
    int vw = winW, vh = winH;
    if (s_post.aspect != 1) {
        if (float(vw) > float(vh) * aspect) vw = int(float(vh) * aspect + 0.5f);
        else vh = int(float(vw) / aspect + 0.5f);
    }
    const int baseH = x.h / s_scale;  // the picture's height at the GameCube's resolution
    if (s_post.aspect == 2 && baseH > 0 && vh >= baseH) {
        vh = vh / baseH * baseH;
        vw = int(float(vh) * aspect + 0.5f);
    }
    int ox = (winW - vw) / 2, oy = (winH - vh) / 2;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    postPresent(x.tex, x.w, x.h, ox, oy, vw, vh);
    glViewport(0, 0, winW, winH);
    // Framebuffer 0 stays bound through the swap: macOS presents nothing
    // (a black window) if an FBO is bound at SDL_GL_SwapWindow.
    // GXPC_EndPresent restores the EFB.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    memset(&s_stats, 0, sizeof(s_stats));
    return 1;
}

void GXPC_EndPresent(void) {
    if (s_ready) glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
}

void GXPC_ReadEFB(uint8_t* rgba, int* w, int* h) {
    flushBatch();
    glcInvalidate();
    *w = s_efbW * s_scale;
    *h = EFB_H * s_scale;
    if (!rgba) return;
    glBindFramebuffer(GL_FRAMEBUFFER, efbReadFbo());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

int GXPC_ReadXFB(const void* xfb, uint8_t* rgba, int* w, int* h) {
    flushBatch();
    glcInvalidate();
    if (!xfb) xfb = s_lastXfb;
    auto it = s_xfbs.find(xfb);
    if (it == s_xfbs.end()) return 0;
    *w = it->second.w;
    *h = it->second.h;
    if (!rgba) return 1;
    glBindFramebuffer(GL_FRAMEBUFFER, s_tmpFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, s_efbFbo);
    return 1;
}

void GXPC_GetLastFrameStats(GXPCStats* out) {
    *out = s_lastFrameStats;
    out->shaderCompiles = g_statShaderCompiles;
    out->textureUploads = g_statTexUploads;
}

void GXPC_SetHud(int on) {
    if (!s_hudEdges || s_hud == (on != 0)) return;
    flushBatch();
    s_hud = on != 0;
    s_hudPanes.clear();
    s_hudAnchor = -1.0f;
}

// Anchor on the outermost pane that is a piece, not a container; the batch
// queued so far keeps its mapping when the anchor moves to another side.
static void hudReanchor() {
    float now = -1.0f;
    for (const HudPane& p : s_hudPanes) {
        if (p.x2 - p.x1 < float(EFB_W) * 3 / 4) {
            now = (p.x1 + p.x2) / 2;
            break;
        }
    }
    if (now == s_hudAnchor) return;
    if (now < 0.0f || s_hudAnchor < 0.0f || hudOffset(now) != hudOffset(s_hudAnchor)) flushBatch();
    s_hudAnchor = now;
}

void GXPC_HudPaneBegin(float x1, float x2) {
    if (!s_hud) return;
    s_hudPanes.push_back({std::min(x1, x2), std::max(x1, x2)});
    hudReanchor();
}

void GXPC_HudPaneEnd(void) {
    if (!s_hud || s_hudPanes.empty()) return;
    s_hudPanes.pop_back();
    hudReanchor();
}

void GXPC_SetStretch2D(int on) {
    if (s_efbW == EFB_W || s_stretch2D == (on != 0)) return;
    flushBatch();  // what was queued keeps its own mapping
    s_stretch2D = on != 0;
}

void GXPC_SetWidescreen(float widthOver43) {
    if (s_ready) return;  // the EFB size is fixed once it exists
    s_wide = widthOver43 > 1.0f ? widthOver43 : 1.0f;
}
float GXPC_GetWidescreen(void) { return s_wide; }

double GXPC_GxSeconds(void) { return s_gxSeconds + g_decodeSeconds; }

void GXPC_GetTimes(GXPCTimes* out) {
    out->gx = s_gxSeconds + g_decodeSeconds;
    out->vertices = g_decodeSeconds;
    out->draws = g_flushSeconds - s_texSeconds;
    out->textures = s_texSeconds;
    out->copies = s_copySeconds;
    out->peeks = s_peekSeconds;
    out->gpuWait = s_waitSeconds;
    out->present = g_presentSeconds;
    out->swap = g_swapSeconds;
    out->idle = s_idleClock ? s_idleClock() : 0;
}

void GXPC_SetDetailedTimers(int on) { g_gxStats = on || statsEnv(); }
void GXPC_SetIdleClock(double (*idleSeconds)(void)) { s_idleClock = idleSeconds; }

void GXPC_DrawOverlay(const uint8_t* rgba, int w, int h, int x, int y, int scale, int winW, int winH) {
    if (!s_ready || w <= 0 || h <= 0) return;
    if (!rgba && (!s_overlayTex || w != s_overlayW || h != s_overlayH)) return;
    flushBatch();
    glcInvalidate();
    // glBlitFramebuffer ignores alpha, so draw the panel with source-alpha blending.
    if (!s_overlayProg) {
        static const char* vs = R"(#version 330 core
uniform vec4 u_rect;
uniform vec2 u_window;
out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4((u_rect.xy + p * u_rect.zw) / u_window * 2.0 - 1.0, 0.0, 1.0);
}
)";
        static const char* fs = R"(#version 330 core
uniform sampler2D u_color;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_color, v_uv); }
)";
        s_overlayProg = compileProgram(vs, fs);
        s_overlayRect = glGetUniformLocation(s_overlayProg, "u_rect");
        s_overlayWindow = glGetUniformLocation(s_overlayProg, "u_window");
    }
    GLint activeUnit = 0, boundTex = 0, rowLength = 0, unpackAlign = 4;
    GLint program = 0, vao = 0, drawFbo = 0, viewport[4], colorMask[4];
    GLint blend = 0, depth = 0, stencil = 0, cull = 0, scissor = 0, logic = 0, clip0 = 0, clip1 = 0;
    GLint srcRGB = 0, dstRGB = 0, srcAlpha = 0, dstAlpha = 0, eqRGB = 0, eqAlpha = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTex);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_COLOR_WRITEMASK, colorMask);
    glGetIntegerv(GL_BLEND, &blend);
    glGetIntegerv(GL_DEPTH_TEST, &depth);
    glGetIntegerv(GL_STENCIL_TEST, &stencil);
    glGetIntegerv(GL_CULL_FACE, &cull);
    glGetIntegerv(GL_SCISSOR_TEST, &scissor);
    glGetIntegerv(GL_COLOR_LOGIC_OP, &logic);
    glGetIntegerv(GL_CLIP_DISTANCE0, &clip0);
    glGetIntegerv(GL_CLIP_DISTANCE1, &clip1);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (!s_overlayTex) {
        glGenTextures(1, &s_overlayTex);
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
    }
    if (rgba) {
        if (w != s_overlayW || h != s_overlayH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            s_overlayW = w;
            s_overlayH = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, winW, winH);
    glUseProgram(s_overlayProg);
    glUniform1i(glGetUniformLocation(s_overlayProg, "u_color"), 0);
    glBindVertexArray(s_copyVao);
    float rect[4] = {float(x), float(winH - y - h * scale), float(w * scale), float(h * scale)};
    float window[2] = {float(winW), float(winH)};
    glUniform4fv(s_overlayRect, 1, rect);
    glUniform2fv(s_overlayWindow, 1, window);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, GLuint(boundTex));
    glActiveTexture(GLenum(activeUnit));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
    glBindVertexArray(GLuint(vao));
    glUseProgram(GLuint(program));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBlendFuncSeparate(GLenum(srcRGB), GLenum(dstRGB), GLenum(srcAlpha), GLenum(dstAlpha));
    glBlendEquationSeparate(GLenum(eqRGB), GLenum(eqAlpha));
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (logic) glEnable(GL_COLOR_LOGIC_OP);
    if (clip0) glEnable(GL_CLIP_DISTANCE0);
    if (clip1) glEnable(GL_CLIP_DISTANCE1);
}

void GXPC_GetStats(GXPCStats* out) {
    *out = s_stats;
    out->shaderCompiles = g_statShaderCompiles;
    out->textureUploads = g_statTexUploads;
}

}  // extern "C"
