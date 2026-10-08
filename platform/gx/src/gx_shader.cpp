// Shader generator: turns the XF (texgen, lighting) and BP (TEV, indirect,
// alpha compare, fog) register state into a GLSL 3.30 program.  Programs are
// cached by the bytes of the state that shapes the code; everything numeric
// (matrices, colours, references) is a uniform.
#include "gx_internal.h"
#include "gl_funcs.h"
#include "gx_glcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>

namespace gx {

extern uint32_t g_statShaderCompiles;

struct ShaderKey {
    uint32_t numTexGens, numChans, dualTex, clip;
    uint32_t texgen[8], post[8];
    uint32_t chan[4];
    uint32_t numStages, numInd;
    uint32_t colorEnv[16], alphaEnv[16], order[16], ksel[16], ind[16];
    uint32_t swap[4];
    uint32_t iref, alphaFunc, fogType, earlyZ, depthOnly;
};

// The qualifier is available in GL 4.2 and as an extension on GL 3.3.
// Older contexts (including macOS) use the ordered depth pass in flushBatch.
static int s_earlyFragmentTests = -1;
bool shaderHasEarlyFragmentTests() {
    if (s_earlyFragmentTests >= 0) return s_earlyFragmentTests != 0;
    s_earlyFragmentTests = 0;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (ext && !strcmp(ext, "GL_ARB_shader_image_load_store")) s_earlyFragmentTests = 1;
    }
    // Also permits exercising the GL 3.3 fallback on newer drivers.
    if (getenv("SMS_GX_FORCE_EARLY_Z_FALLBACK")) s_earlyFragmentTests = 0;
    return s_earlyFragmentTests != 0;
}

// GXSetZCompLoc(GX_TRUE) differs from a late depth test only for a draw that
// writes depth and whose alpha test can reject: the rejected pixels still
// write depth. Early fragment tests are kept to those draws because they also
// count occlusion-query samples before the alpha test's discard, and the
// pollution counters read pixel metrics with ReInitializeGX's early Z left on.
bool earlyZWritesRejected() {
    uint32_t alphaFunc = (g.bp[BP_ALPHACOMPARE] >> 16) & 0xFF;
    return (g.bp[BP_PE_CONTROL] & (1 << 6)) && (g.bp[BP_ZMODE] & 0x11) == 0x11 && alphaFunc != 0x3F &&
           alphaFunc != 0x7F;
}

static void buildKey(ShaderKey& k) {
    memset(&k, 0, sizeof(k));
    k.numTexGens = g.xfReg[XFR_NUMTEXGENS] & 15;
    if (k.numTexGens > 8) k.numTexGens = 8;
    k.numChans = g.xfReg[XFR_NUMCHANS] & 3;
    k.dualTex = g.xfReg[XFR_DUALTEX] & 1;
    k.clip = g.xfReg[0x05] == 0;
    for (uint32_t i = 0; i < k.numTexGens; i++) {
        k.texgen[i] = g.xfReg[XFR_TEXGEN + i];
        if (k.dualTex) k.post[i] = g.xfReg[XFR_POSTTEX + i] & 0x13F;
    }
    for (uint32_t i = 0; i < 2; i++) {
        k.chan[i] = g.xfReg[XFR_COLOR0CTRL + i] & 0x7FFF;
        k.chan[2 + i] = g.xfReg[XFR_ALPHA0CTRL + i] & 0x7FFF;
    }
    uint32_t gen = g.bp[BP_GENMODE];
    k.numStages = ((gen >> 10) & 15) + 1;
    k.numInd = (gen >> 16) & 7;
    if (k.numInd > 4) k.numInd = 4;
    for (uint32_t s = 0; s < k.numStages; s++) {
        k.colorEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFF;
        k.alphaEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFF;
        k.order[s] = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
        k.ksel[s] = (s & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
        k.ind[s] = k.numInd ? (g.bp[BP_IND_CMD + s] & 0x1FFFFF) : 0;
    }
    for (int t = 0; t < 4; t++)
        k.swap[t] = (g.bp[BP_TEV_KSEL + 2 * t] & 15) | (g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4;
    k.iref = k.numInd ? g.bp[BP_RAS1_IREF] & 0xFFFFFF : 0;
    k.alphaFunc = (g.bp[BP_ALPHACOMPARE] >> 16) & 0xFF;
    k.earlyZ = earlyZWritesRejected() && shaderHasEarlyFragmentTests();
    k.fogType = (g.bp[BP_FOG3] >> 21) & 7;
}

// ------------------------------------------------------------------ vertex shader
static const char* kVsHeader = R"(#version 330 core
layout(std140) uniform XFBlock { uvec4 xf[184]; };
uniform vec4 u_proj[2];
uniform vec4 u_vp[2];
uniform vec2 u_efb;
uniform vec4 u_chan[4];
in vec3 a_pos; in vec3 a_nrm; in vec3 a_bin; in vec3 a_tan;
in vec4 a_clr0; in vec4 a_clr1;
in vec2 a_tex0; in vec2 a_tex1; in vec2 a_tex2; in vec2 a_tex3;
in vec2 a_tex4; in vec2 a_tex5; in vec2 a_tex6; in vec2 a_tex7;
in uvec3 a_mtx;
out vec4 v_c0; out vec4 v_c1;
out vec3 v_tc0; out vec3 v_tc1; out vec3 v_tc2; out vec3 v_tc3;
out vec3 v_tc4; out vec3 v_tc5; out vec3 v_tc6; out vec3 v_tc7;
out float v_depth;
float X(int i) { return uintBitsToFloat(xf[i >> 2][i & 3]); }
vec4 R4(int w) { return vec4(X(w), X(w + 1), X(w + 2), X(w + 3)); }
vec3 R3(int w) { return vec3(X(w), X(w + 1), X(w + 2)); }
vec4 C(int w) { uint u = xf[w >> 2][w & 3];
  return vec4(float((u >> 24) & 255u), float((u >> 16) & 255u), float((u >> 8) & 255u), float(u & 255u)) / 255.0; }
// light block: word 608 + 16 * light
vec4 lightColor(int l) { return C(608 + l * 16 + 3); }
vec3 lightA(int l) { return R3(608 + l * 16 + 4); }
vec3 lightK(int l) { return R3(608 + l * 16 + 7); }
vec3 lightPos(int l) { return R3(608 + l * 16 + 10); }
vec3 lightDir(int l) { return R3(608 + l * 16 + 13); }
)";

// Attenuation divides by k0 + k1*d + k2*d^2.  A light loaded with all-zero
// coefficients (J3D leaves unused lights like that) must contribute nothing
// rather than 0/0 = NaN, which would turn the whole channel black; a zero
// denominator with a positive numerator saturates, as the hardware reciprocal does.
static void emitLight(std::string& s, uint32_t ctrl, const char* comp, const char* sumName, int light) {
    char buf[1024];
    std::string sumS = std::string(sumName) + comp;
    const char* sum = sumS.c_str();
    uint32_t diff = (ctrl >> 7) & 3;
    bool attn = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
    const char* dfn = diff == 0 ? "1.0" : diff == 1 ? "dot(N, L)" : "max(dot(N, L), 0.0)";
    if (!attn) {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = normalize(lightPos(%d) - vpos); %s += (%s) * lightColor(%d)%s; }\n", light, sum, dfn,
                 light, comp);
    } else if (spot) {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = lightPos(%d) - vpos; float d2 = dot(L, L); float d = sqrt(d2); L /= d;\n"
                 "    float cs = max(dot(L, lightDir(%d)), 0.0); vec3 a = lightA(%d); vec3 k = lightK(%d);\n"
                 "    float at = max(a.x + a.y * cs + a.z * cs * cs, 0.0) / max(k.x + k.y * d + k.z * d2, 1e-30);\n"
                 "    %s += at * (%s) * lightColor(%d)%s; }\n",
                 light, light, light, light, sum, dfn, light, comp);
    } else {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = normalize(lightPos(%d)); float nl = dot(N, L);\n"
                 "    float t = nl > 0.0 ? max(dot(N, lightDir(%d)), 0.0) : 0.0; vec3 a = lightA(%d); vec3 k = lightK(%d);\n"
                 "    float at = max(a.x + a.y * t + a.z * t * t, 0.0) / max(k.x + k.y * t + k.z * t * t, 1e-30);\n"
                 "    %s += at * (%s) * lightColor(%d)%s; }\n",
                 light, light, light, light, sum, dfn, light, comp);
    }
    s += buf;
}

static std::string genVS(const ShaderKey& k) {
    std::string s = kVsHeader;
    char buf[1024];
    s += "void main() {\n"
         "  int pm = int(a_mtx.x & 255u);\n"
         "  vec4 p = vec4(a_pos, 1.0);\n"
         "  vec3 vpos = vec3(dot(R4(pm * 4), p), dot(R4(pm * 4 + 4), p), dot(R4(pm * 4 + 8), p));\n"
         "  int nm = 256 + pm * 3;\n"
         "  vec3 N = vec3(dot(R3(nm), a_nrm), dot(R3(nm + 3), a_nrm), dot(R3(nm + 6), a_nrm));\n"
         "  N = dot(N, N) > 0.0 ? normalize(N) : vec3(0.0, 0.0, 1.0);\n"
         "  vec4 clip;\n"
         "  if (u_proj[1].z == 0.0) {\n"
         "    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y * vpos.z, u_proj[0].z * vpos.y + u_proj[0].w * vpos.z,\n"
         "                u_proj[1].x * vpos.z + u_proj[1].y, -vpos.z);\n"
         "  } else {\n"
         "    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y, u_proj[0].z * vpos.y + u_proj[0].w,\n"
         "                u_proj[1].x * vpos.z + u_proj[1].y, 1.0);\n"
         "  }\n"
         // viewport in EFB pixels, then to GL clip space over the whole EFB (row 0 = EFB top)
         "  gl_Position.x = ((u_vp[1].x - 342.0) * clip.w + u_vp[0].x * clip.x) * (2.0 / u_efb.x) - clip.w;\n"
         "  gl_Position.y = ((u_vp[1].y - 342.0) * clip.w + u_vp[0].y * clip.y) * (2.0 / u_efb.y) - clip.w;\n"
         "  float zw = u_vp[1].z * clip.w + u_vp[0].z * clip.z;\n"
         "  gl_Position.z = zw * (2.0 / 16777215.0) - clip.w;\n"
         "  gl_Position.w = clip.w;\n"
         "  v_depth = zw / (16777215.0 * clip.w);\n";
    if (k.clip) s += "  gl_ClipDistance[0] = clip.z + clip.w;\n  gl_ClipDistance[1] = -clip.z;\n";
    else s += "  gl_ClipDistance[0] = 1.0;\n  gl_ClipDistance[1] = 1.0;\n";

    // lighting: channels 0/1, colour and alpha controlled separately
    const char* vtx[2] = {"a_clr0", "a_clr1"};
    for (int c = 0; c < 2; c++) {
        uint32_t cc = k.chan[c], ac = k.chan[2 + c];
        snprintf(buf, sizeof buf, "  vec4 col%d;\n", c);
        s += buf;
        if (uint32_t(c) >= k.numChans) {
            snprintf(buf, sizeof buf, "  col%d = vec4(0.0);\n", c);
            s += buf;
            continue;
        }
        for (int part = 0; part < 2; part++) {
            uint32_t ctrl = part ? ac : cc;
            const char* comp = part ? ".a" : ".rgb";
            snprintf(buf, sizeof buf, "  { vec4 mat = %s; ", (ctrl & 1) ? vtx[c] : (c ? "u_chan[3]" : "u_chan[2]"));
            s += buf;
            if (ctrl & 2) {
                snprintf(buf, sizeof buf, "vec4 amb = %s; vec4 lsum = amb;\n", (ctrl & 64) ? vtx[c] : (c ? "u_chan[1]" : "u_chan[0]"));
                s += buf;
                uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
                for (int l = 0; l < 8; l++)
                    if (mask & (1u << l)) emitLight(s, ctrl, comp, "lsum", l);
                snprintf(buf, sizeof buf, "    col%d%s = (mat * clamp(lsum, 0.0, 1.0))%s; }\n", c, comp, comp);
            } else {
                snprintf(buf, sizeof buf, "col%d%s = mat%s; }\n", c, comp, comp);
            }
            s += buf;
        }
    }
    s += "  v_c0 = col0;\n  v_c1 = col1;\n";

    // texgen
    for (uint32_t i = 0; i < 8; i++) {
        if (i >= k.numTexGens) {
            snprintf(buf, sizeof buf, "  v_tc%u = vec3(a_tex%u, 1.0);\n", i, i);
            s += buf;
            continue;
        }
        uint32_t tg = k.texgen[i];
        uint32_t proj = (tg >> 1) & 1, form = (tg >> 2) & 1, type = (tg >> 4) & 7, row = (tg >> 7) & 31;
        uint32_t embSrc = (tg >> 12) & 7;
        std::string src;
        if (row == 0) src = "vec4(a_pos, 1.0)";
        else if (row == 1) src = "vec4(a_nrm, 1.0)";
        else if (row == 2) src = "a_clr0";
        else if (row == 3) src = "vec4(a_bin, 1.0)";
        else if (row == 4) src = "vec4(a_tan, 1.0)";
        else {
            snprintf(buf, sizeof buf, "vec4(a_tex%u, 1.0, 1.0)", (row - 5) & 7);
            src = buf;
        }
        if (form == 0 && row >= 5) {
            // AB11: third component forced to 1
        } else if (form == 0) {
            src = "vec4((" + src + ").xy, 1.0, 1.0)";
        }
        // texture matrix index for this coordinate
        const char* mi[8] = {"(a_mtx.x >> 8) & 255u", "(a_mtx.x >> 16) & 255u", "a_mtx.x >> 24", "a_mtx.y & 255u",
                              "(a_mtx.y >> 8) & 255u", "(a_mtx.y >> 16) & 255u", "a_mtx.y >> 24", "a_mtx.z & 255u"};
        if (type == 0) {
            snprintf(buf, sizeof buf,
                     "  { vec4 sv = %s; int tm = int(%s) * 4; vec3 t = vec3(dot(R4(tm), sv), dot(R4(tm + 4), sv), %s);\n",
                     src.c_str(), mi[i], proj ? "dot(R4(tm + 8), sv)" : "1.0");
            s += buf;
        } else if (type == 1) {
            // emboss: offset of the source coordinate (approximated as the source itself)
            snprintf(buf, sizeof buf, "  { vec3 t = v_tc%u;\n", embSrc < i ? embSrc : 0);
            s += buf;
        } else {
            snprintf(buf, sizeof buf, "  { vec3 t = vec3(%s.rg, 1.0);\n", type == 3 ? "col1" : "col0");
            s += buf;
        }
        if (k.dualTex && type == 0) {
            uint32_t pt = k.post[i] & 63;
            bool norm = (k.post[i] >> 8) & 1;
            snprintf(buf, sizeof buf,
                     "    %s int pw = 352 + %u * 4; vec4 pv = vec4(t, 1.0);\n"
                     "    t = vec3(dot(R4(pw), pv), dot(R4(pw + 4), pv), dot(R4(pw + 8), pv));\n",
                     norm ? "t = normalize(t);" : "", pt);
            s += buf;
        }
        snprintf(buf, sizeof buf, "    v_tc%u = t; }\n", i);
        s += buf;
    }
    s += "}\n";
    return s;
}

// ------------------------------------------------------------------ fragment shader
static const char* kFsHeader = R"(#version 330 core
in vec4 v_c0; in vec4 v_c1;
in vec3 v_tc0; in vec3 v_tc1; in vec3 v_tc2; in vec3 v_tc3;
in vec3 v_tc4; in vec3 v_tc5; in vec3 v_tc6; in vec3 v_tc7;
in float v_depth;
uniform sampler2D u_tex[8];
uniform ivec4 u_tevreg[4];
uniform ivec4 u_konst[4];
uniform vec2 u_texscale[8];
uniform vec2 u_texsize[8];
uniform ivec2 u_alpharef;
uniform vec4 u_fog;
uniform vec4 u_fogcolor;
uniform vec4 u_indmtx[6];
out vec4 o_color;
ivec4 texi(vec4 c) { return ivec4(round(clamp(c, 0.0, 1.0) * 255.0)); }
vec2 proj(vec3 t) { return t.xy / (t.z == 0.0 ? 1.0 : t.z); }
ivec3 lerp3(ivec3 a, ivec3 b, ivec3 c) { ivec3 c2 = c + (c >> 7); return (a * (256 - c2) + b * c2 + 128) >> 8; }
int lerp1(int a, int b, int c) { int c2 = c + (c >> 7); return (a * (256 - c2) + b * c2 + 128) >> 8; }
)";

static const char* kColorArg[16] = {"cprev.rgb", "cprev.aaa", "creg0.rgb", "creg0.aaa", "creg1.rgb", "creg1.aaa",
                                    "creg2.rgb", "creg2.aaa", "tex.rgb",   "tex.aaa",   "ras.rgb",   "ras.aaa",
                                    "ivec3(255)", "ivec3(128)", "konst.rgb", "ivec3(0)"};
static const char* kAlphaArg[8] = {"cprev.a", "creg0.a", "creg1.a", "creg2.a", "tex.a", "ras.a", "konst.a", "0"};
static const char* kRegName[4] = {"cprev", "creg0", "creg1", "creg2"};

static std::string konstExpr(uint32_t sel, bool alpha) {
    static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    char buf[64];
    if (sel < 8) {
        snprintf(buf, sizeof buf, alpha ? "%d" : "ivec3(%d)", k8[sel]);
        return buf;
    }
    if (!alpha && sel >= 12 && sel < 16) {
        snprintf(buf, sizeof buf, "u_konst[%u].rgb", sel - 12);
        return buf;
    }
    if (sel >= 16) {
        const char comp = "rgba"[(sel - 16) >> 2];
        snprintf(buf, sizeof buf, alpha ? "u_konst[%u].%c" : "ivec3(u_konst[%u].%c)", sel & 3, comp);
        return buf;
    }
    return alpha ? "255" : "ivec3(255)";
}

static std::string swizzle(uint32_t swap) {
    std::string s;
    const char* c = "rgba";
    s += c[swap & 3];
    s += c[(swap >> 2) & 3];
    s += c[(swap >> 4) & 3];
    s += c[(swap >> 6) & 3];
    return s;
}

static std::string genFS(const ShaderKey& k) {
    std::string s = kFsHeader;
    if (k.earlyZ) {
        s.insert(s.find('\n') + 1, "#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n");
    }
    if (k.depthOnly) return s + "void main() { o_color = vec4(0.0); }\n";
    char buf[2048];
    s += "void main() {\n"
         "  ivec4 cprev = u_tevreg[0], creg0 = u_tevreg[1], creg1 = u_tevreg[2], creg2 = u_tevreg[3];\n"
         "  ivec4 ras0 = texi(v_c0), ras1 = texi(v_c1);\n"
         "  ivec4 tex = ivec4(255), ras = ivec4(0), konst = ivec4(0);\n"
         "  int bumpA = 0; vec2 prevTc = vec2(0.0);\n";
    // indirect texture lookups
    for (uint32_t i = 0; i < k.numInd; i++) {
        uint32_t map = (k.iref >> (6 * i)) & 7, coord = (k.iref >> (6 * i + 3)) & 7;
        uint32_t ss = (g.bp[i < 2 ? BP_RAS1_SS0 : BP_RAS1_SS1] >> ((i & 1) * 8)) & 0xFF;
        snprintf(buf, sizeof buf,
                 "  ivec3 ind%u = texi(texture(u_tex[%u], proj(v_tc%u) / vec2(%d.0, %d.0))).abg;\n", i, map, coord,
                 1 << (ss & 15), 1 << ((ss >> 4) & 15));
        s += buf;
    }
    uint32_t lastDest = 0;
    bool lastAlphaDest = 0;
    uint32_t lastA = 0;
    for (uint32_t st = 0; st < k.numStages; st++) {
        uint32_t ord = k.order[st];
        uint32_t map = ord & 7, coord = (ord >> 3) & 7, en = (ord >> 6) & 1, chan = (ord >> 7) & 7;
        uint32_t ce = k.colorEnv[st], ae = k.alphaEnv[st];
        uint32_t rasSwap = k.swap[ae & 3], texSwap = k.swap[(ae >> 2) & 3];
        snprintf(buf, sizeof buf, "  // stage %u\n  {\n", st);
        s += buf;
        uint32_t ind = k.ind[st];
        if (en) {
            if (ind) {
                uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3,
                         mid = (ind >> 9) & 15, sw = (ind >> 13) & 7, tw = (ind >> 16) & 7, fb = (ind >> 20) & 1;
                static const int fmtMask[4] = {255, 31, 15, 7};
                snprintf(buf, sizeof buf, "    ivec3 iv = ind%u & ivec3(%d);\n", bt < k.numInd ? bt : 0, fmtMask[fmt]);
                s += buf;
                if (bs) {
                    snprintf(buf, sizeof buf, "    bumpA = (ind%u.%c) & 0xF8;\n", bt < k.numInd ? bt : 0, "xxyz"[bs]);
                    s += buf;
                }
                int bv = fmt == 0 ? -128 : 1;
                snprintf(buf, sizeof buf, "    iv += ivec3(%d, %d, %d);\n", (bias & 1) ? bv : 0, (bias & 2) ? bv : 0, (bias & 4) ? bv : 0);
                s += buf;
                if (mid >= 1 && mid <= 3) {
                    uint32_t m = mid - 1;
                    snprintf(buf, sizeof buf,
                             "    vec2 off = vec2(dot(u_indmtx[%u].xyz, vec3(iv)), dot(u_indmtx[%u].xyz, vec3(iv))) * u_indmtx[%u].w;\n",
                             m * 2, m * 2 + 1, m * 2);
                } else {
                    snprintf(buf, sizeof buf, "    vec2 off = vec2(0.0);\n");
                }
                s += buf;
                snprintf(buf, sizeof buf, "    vec2 tcx = proj(v_tc%u) * u_texscale[%u] * u_texsize[%u];\n", coord, coord, map);
                s += buf;
                static const int wrapSize[8] = {0, 256, 128, 64, 32, 16, 0, 0};
                if (sw == 6) s += "    tcx.x = 0.0;\n";
                else if (sw) { snprintf(buf, sizeof buf, "    tcx.x = mod(tcx.x, %d.0);\n", wrapSize[sw]); s += buf; }
                if (tw == 6) s += "    tcx.y = 0.0;\n";
                else if (tw) { snprintf(buf, sizeof buf, "    tcx.y = mod(tcx.y, %d.0);\n", wrapSize[tw]); s += buf; }
                snprintf(buf, sizeof buf, "    tcx += off%s; prevTc = tcx;\n", fb ? " + prevTc" : "");
                s += buf;
                snprintf(buf, sizeof buf, "    tex = texi(texture(u_tex[%u], tcx / u_texsize[%u])).%s;\n", map, map, swizzle(texSwap).c_str());
            } else {
                snprintf(buf, sizeof buf, "    tex = texi(texture(u_tex[%u], proj(v_tc%u) * u_texscale[%u])).%s;\n", map, coord,
                         coord, swizzle(texSwap).c_str());
            }
            s += buf;
        }
        if (chan == 0) snprintf(buf, sizeof buf, "    ras = ras0.%s;\n", swizzle(rasSwap).c_str());
        else if (chan == 1) snprintf(buf, sizeof buf, "    ras = ras1.%s;\n", swizzle(rasSwap).c_str());
        else if (chan == 5) snprintf(buf, sizeof buf, "    ras = ivec4(bumpA);\n");
        else if (chan == 6) snprintf(buf, sizeof buf, "    ras = ivec4(bumpA | (bumpA >> 5));\n");
        else snprintf(buf, sizeof buf, "    ras = ivec4(0);\n");
        s += buf;
        uint32_t kc = k.ksel[st] & 31, ka = (k.ksel[st] >> 5) & 31;
        snprintf(buf, sizeof buf, "    konst = ivec4(%s, %s);\n", konstExpr(kc, false).c_str(), konstExpr(ka, true).c_str());
        s += buf;

        // colour
        {
            uint32_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
            uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clampv = (ce >> 19) & 1, scale = (ce >> 20) & 3,
                     dest = (ce >> 22) & 3;
            snprintf(buf, sizeof buf,
                     "    ivec3 A = (%s) & 255, B = (%s) & 255, Cc = (%s) & 255, D = %s;\n", kColorArg[a], kColorArg[b],
                     kColorArg[c], kColorArg[d]);
            s += buf;
            if (bias != 3) {
                snprintf(buf, sizeof buf, "    ivec3 r = D %c lerp3(A, B, Cc)%s;\n", op ? '-' : '+',
                         bias == 1 ? " + 128" : bias == 2 ? " - 128" : "");
                s += buf;
                if (scale == 1) s += "    r *= 2;\n";
                else if (scale == 2) s += "    r *= 4;\n";
                else if (scale == 3) s += "    r = r >> 1;\n";
            } else {
                uint32_t mode = (scale << 1) | op;
                const char* cond;
                switch (mode) {
                case 0: cond = "bvec3(A.r > B.r)"; break;
                case 1: cond = "bvec3(A.r == B.r)"; break;
                case 2: cond = "bvec3((A.g * 256 + A.r) > (B.g * 256 + B.r))"; break;
                case 3: cond = "bvec3((A.g * 256 + A.r) == (B.g * 256 + B.r))"; break;
                case 4: cond = "bvec3((A.b * 65536 + A.g * 256 + A.r) > (B.b * 65536 + B.g * 256 + B.r))"; break;
                case 5: cond = "bvec3((A.b * 65536 + A.g * 256 + A.r) == (B.b * 65536 + B.g * 256 + B.r))"; break;
                case 6: cond = "greaterThan(A, B)"; break;
                default: cond = "equal(A, B)"; break;
                }
                snprintf(buf, sizeof buf, "    ivec3 r = D + ivec3(%s) * Cc;\n", cond);
                s += buf;
            }
            snprintf(buf, sizeof buf, "    %s.rgb = %s;\n", kRegName[dest],
                     clampv ? "clamp(r, 0, 255)" : "clamp(r, -1024, 1023)");
            s += buf;
            lastDest = dest;
        }
        // alpha
        {
            uint32_t d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
            uint32_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clampv = (ae >> 19) & 1, scale = (ae >> 20) & 3,
                     dest = (ae >> 22) & 3;
            snprintf(buf, sizeof buf, "    int Aa = (%s) & 255, Ba = (%s) & 255, Ca = (%s) & 255, Da = %s;\n",
                     kAlphaArg[a], kAlphaArg[b], kAlphaArg[c], kAlphaArg[d]);
            s += buf;
            if (bias != 3) {
                snprintf(buf, sizeof buf, "    int ra = Da %c lerp1(Aa, Ba, Ca)%s;\n", op ? '-' : '+',
                         bias == 1 ? " + 128" : bias == 2 ? " - 128" : "");
                s += buf;
                if (scale == 1) s += "    ra *= 2;\n";
                else if (scale == 2) s += "    ra *= 4;\n";
                else if (scale == 3) s += "    ra = ra >> 1;\n";
            } else {
                uint32_t mode = (scale << 1) | op;
                const char* cond;
                switch (mode) {
                case 0: cond = "A.r > B.r"; break;
                case 1: cond = "A.r == B.r"; break;
                case 2: cond = "(A.g * 256 + A.r) > (B.g * 256 + B.r)"; break;
                case 3: cond = "(A.g * 256 + A.r) == (B.g * 256 + B.r)"; break;
                case 4: cond = "(A.b * 65536 + A.g * 256 + A.r) > (B.b * 65536 + B.g * 256 + B.r)"; break;
                case 5: cond = "(A.b * 65536 + A.g * 256 + A.r) == (B.b * 65536 + B.g * 256 + B.r)"; break;
                case 6: cond = "Aa > Ba"; break;
                default: cond = "Aa == Ba"; break;
                }
                snprintf(buf, sizeof buf, "    int ra = Da + ((%s) ? Ca : 0);\n", cond);
                s += buf;
            }
            snprintf(buf, sizeof buf, "    %s.a = %s;\n", kRegName[dest], clampv ? "clamp(ra, 0, 255)" : "clamp(ra, -1024, 1023)");
            s += buf;
            lastA = dest;
            lastAlphaDest = true;
        }
        s += "  }\n";
    }
    (void)lastAlphaDest;
    // The last stage's output keeps its low 8 bits, as on the console (and in
    // Dolphin): an unclamped stage can wrap. Bianco Hills' river surface
    // computes its alpha as (1 + a) * 2 and relies on the wrap to get 2a back;
    // clamped, it is 255 and the additive layer washes the water out.
    snprintf(buf, sizeof buf, "  ivec4 outc = ivec4(%s.rgb, %s.a) & 255;\n", kRegName[lastDest], kRegName[lastA]);
    s += buf;

    // alpha compare
    {
        uint32_t c0 = k.alphaFunc & 7, c1 = (k.alphaFunc >> 3) & 7, op = (k.alphaFunc >> 6) & 3;
        auto cmp = [](uint32_t f, const char* ref) -> std::string {
            static const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
            if (f == 0) return "false";
            if (f == 7) return "true";
            return std::string("(outc.a ") + ops[f] + " " + ref + ")";
        };
        std::string a = cmp(c0, "u_alpharef.x"), b = cmp(c1, "u_alpharef.y");
        std::string e;
        switch (op) {
        case 0: e = a + " && " + b; break;
        case 1: e = a + " || " + b; break;
        case 2: e = a + " != " + b; break;
        default: e = a + " == " + b; break;
        }
        if (!(c0 == 7 && c1 == 7 && op <= 1)) s += "  if (!(" + e + ")) discard;\n";
    }
    s += "  vec4 color = vec4(outc) / 255.0;\n";
    if (k.fogType) {
        // u_fog: x = A, y = B, z = C, w = 1 for orthographic
        s += "  float ze = u_fog.w != 0.0 ? u_fog.x * v_depth : u_fog.x / (u_fog.y - v_depth);\n"
             "  float f = clamp(ze - u_fog.z, 0.0, 1.0);\n";
        switch (k.fogType) {
        case 4: s += "  f = 1.0 - exp2(-8.0 * f);\n"; break;
        case 5: s += "  f = 1.0 - exp2(-8.0 * f * f);\n"; break;
        case 6: s += "  f = exp2(-8.0 * (1.0 - f));\n"; break;
        case 7: s += "  f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"; break;
        default: break;
        }
        s += "  color.rgb = mix(color.rgb, u_fogcolor.rgb, f);\n";
    }
    s += "  o_color = color;\n}\n";
    return s;
}

// ------------------------------------------------------------------ compile / cache
static GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        logmsg("shader compile failed:\n%s\n--- source ---\n%s", log, src.c_str());
    }
    return sh;
}

// The program cache key: the shader key plus the indirect scale factors,
// which are baked into the code. Compared as bytes (buildKey zero-fills).
struct ProgramKey {
    ShaderKey k;
    uint32_t indScale[2];
    bool operator==(const ProgramKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct ProgramKeyHash {
    size_t operator()(const ProgramKey& k) const { return size_t(hashBytes(&k, sizeof(k))); }
};
static std::unordered_map<ProgramKey, ShaderProgram, ProgramKeyHash> s_programs;
static ProgramKey s_lastKey;
static const ShaderProgram* s_lastProgram = nullptr;

const ShaderProgram* shaderForCurrentState(bool depthOnly) {
    ProgramKey key;
    ShaderKey& k = key.k;
    buildKey(k);
    k.depthOnly = depthOnly;
    key.indScale[0] = k.numInd ? g.bp[BP_RAS1_SS0] : 0;
    key.indScale[1] = k.numInd ? g.bp[BP_RAS1_SS0 + 1] : 0;
    // consecutive batches mostly share a program
    if (s_lastProgram && key == s_lastKey) return s_lastProgram;
    auto it = s_programs.find(key);
    if (it != s_programs.end()) {
        s_lastKey = key;
        return s_lastProgram = &it->second;
    }

    g_statShaderCompiles++;
    std::string vs = genVS(k), fs = genFS(k);
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    static const char* attrs[] = {"a_pos", "a_nrm", "a_bin", "a_tan", "a_clr0", "a_clr1", "a_tex0", "a_tex1", "a_tex2",
                                  "a_tex3", "a_tex4", "a_tex5", "a_tex6", "a_tex7", "a_mtx"};
    for (GLuint i = 0; i < 15; i++) glBindAttribLocation(p, i, attrs[i]);
    glBindFragDataLocation(p, 0, "o_color");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        logmsg("program link failed: %s", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    ShaderProgram sp;
    sp.prog = p;
    sp.uc.init = false;
    sp.id = int(g_statShaderCompiles);
    if (const char* dir = getenv("SMS_GX_DUMP_SHADERS")) {
        char path[1024];
        snprintf(path, sizeof path, "%s/prog%d.vs", dir, sp.id);
        if (FILE* f = fopen(path, "w")) { fputs(vs.c_str(), f); fclose(f); }
        snprintf(path, sizeof path, "%s/prog%d.fs", dir, sp.id);
        if (FILE* f = fopen(path, "w")) { fputs(fs.c_str(), f); fclose(f); }
    }
    glUseProgram(p);
    g_glc.prog = p;
    GLuint blk = glGetUniformBlockIndex(p, "XFBlock");
    if (blk != GL_INVALID_INDEX) glUniformBlockBinding(p, blk, 0);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    GLint loc = glGetUniformLocation(p, "u_tex");
    if (loc >= 0) glUniform1iv(loc, 8, units);
    sp.uTevReg = glGetUniformLocation(p, "u_tevreg");
    sp.uKonst = glGetUniformLocation(p, "u_konst");
    sp.uTexScale = glGetUniformLocation(p, "u_texscale");
    sp.uAlphaRef = glGetUniformLocation(p, "u_alpharef");
    sp.uFog = glGetUniformLocation(p, "u_fog");
    sp.uFogColor = glGetUniformLocation(p, "u_fogcolor");
    sp.uIndMtx = glGetUniformLocation(p, "u_indmtx");
    sp.uTexSize = glGetUniformLocation(p, "u_texsize");
    sp.uEfb = glGetUniformLocation(p, "u_efb");
    sp.uProj = glGetUniformLocation(p, "u_proj");
    sp.uViewport = glGetUniformLocation(p, "u_vp");
    sp.uAmbMat = glGetUniformLocation(p, "u_chan");
    sp.uDstAlpha = -1;
    s_lastKey = key;
    return s_lastProgram = &(s_programs[key] = sp);
}

void shaderShutdown() {
    for (auto& kv : s_programs) glDeleteProgram(kv.second.prog);
    s_programs.clear();
    s_lastProgram = nullptr;
    s_earlyFragmentTests = -1;
}

}  // namespace gx
