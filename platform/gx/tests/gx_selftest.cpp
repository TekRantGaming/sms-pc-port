// Headless self-test for sms_gx: creates an OpenGL 3.3 core context through
// EGL (surfaceless Mesa, falling back to the default display), drives the GX
// and GD APIs the way game code does and checks EFB/XFB pixels.
//
//   gx_selftest [--headless|--window] [out.ppm]
//     --window shows the final frame for two seconds; out.ppm receives the EFB
//
// GXVert.h is replaced by sms_gx/gxvert_pc.h (included first, it claims the
// header guard), which is the TARGET_PC configuration the port uses.
#include <sms_gx/gxvert_pc.h>
#include "gd_host_prefix.h"  // __cntlzw for GDLight.h
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/mtx.h>

#include "sms_gx/gx_pc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace gx {
void decodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutFmt,
                   uint8_t* out);
}

// The OS layer normally provides these.
void OSPanic(const char* file, int line, const char* msg, ...) {
    fprintf(stderr, "OSPanic %s:%d %s\n", file, line, msg);
    abort();
}
void DCFlushRange(void* p, u32 n) { GXPC_InvalidateRange(p, uint32_t(n)); }

static int s_fail = 0, s_pass = 0;

static bool near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }

static void expectPixel(const char* what, int x, int y, int r, int g, int b, int tol = 2) {
    u32 c = 0;
    GXPeekARGB(u16(x), u16(y), &c);
    int pr = int((c >> 16) & 255), pg = int((c >> 8) & 255), pb = int(c & 255);
    bool ok = near(pr, r, tol) && near(pg, g, tol) && near(pb, b, tol);
    printf("%s %-44s (%3d,%3d) got %3d %3d %3d want %3d %3d %3d\n", ok ? "PASS" : "FAIL", what, x, y, pr, pg, pb, r, g, b);
    ok ? s_pass++ : s_fail++;
}

static void expect(const char* what, bool ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? s_pass++ : s_fail++;
}

// ------------------------------------------------------------------ helpers
static void setOrtho() {
    Mtx44 m;
    memset(m, 0, sizeof(m));
    // left 0, right 640, top 0, bottom 480, near 0, far 10
    m[0][0] = 2.0f / 640;
    m[0][3] = -1.0f;
    m[1][1] = 2.0f / -480;
    m[1][3] = 1.0f;
    m[2][2] = -1.0f / 10;
    m[2][3] = -1.0f;
    m[3][3] = 1.0f;
    GXSetProjection(m, GX_ORTHOGRAPHIC);
    Mtx id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(id, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

static void colorQuad(float x0, float y0, float x1, float y1, u8 r, u8 g, u8 b, u8 a, float z = -1) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y0, z); GXColor4u8(r, g, b, a);
    GXPosition3f32(x1, y0, z); GXColor4u8(r, g, b, a);
    GXPosition3f32(x1, y1, z); GXColor4u8(r, g, b, a);
    GXPosition3f32(x0, y1, z); GXColor4u8(r, g, b, a);
    GXEnd();
}

static void colorSetup() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
}

// Encodes an RGBA8 image (w, h multiples of 4) into GX RGBA8 tiles.
static std::vector<u8> encodeRGBA8(const u8* rgba, int w, int h) {
    std::vector<u8> out(size_t(w) * h * 4);
    size_t o = 0;
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4) {
            for (int i = 0; i < 16; i++) {
                const u8* p = rgba + ((ty + i / 4) * w + tx + i % 4) * 4;
                out[o + 2 * i] = p[3];
                out[o + 2 * i + 1] = p[0];
                out[o + 32 + 2 * i] = p[1];
                out[o + 32 + 2 * i + 1] = p[2];
            }
            o += 64;
        }
    return out;
}

static void texQuad(float x0, float y0, float x1, float y1, u8 r, u8 g, u8 b, u8 a) {
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    GXPosition3f32(x0, y0, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(0, 0);
    GXPosition3f32(x1, y0, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(1, 0);
    GXPosition3f32(x1, y1, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(1, 1);
    GXPosition3f32(x0, y1, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(0, 1);
    GXEnd();
}

static void texSetup() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
}

int main(int argc, char** argv) {
    // Assertions need the current EFB, rather than the game's pipelined peeks.
#ifdef _WIN32
    _putenv_s("SMS_GX_SYNC_READS", "1");
#else
    setenv("SMS_GX_SYNC_READS", "1", 1);
#endif
    // default to offscreen; `gx_selftest --window` shows the result instead
    GXPC_SetHeadless(1);
    GXPC_ParseArgs(&argc, argv);
    GXPC_SetAutoPresent(0);
    if (!GXPC_InitAuto(1)) {
        fprintf(stderr, "no OpenGL 3.3 context available; skipping\n");
        return 77;
    }
    // emulated main memory: command streams carry 32-bit offsets into it
    static u8 arena[8 << 20] __attribute__((aligned(32)));
    GXPC_SetMemoryWindow(arena, sizeof(arena));
    static u8 fifo[256 * 1024];
    GXInit(fifo, sizeof(fifo));
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    setOrtho();

    static u8 xfb[640 * 480 * 2];
    GXColor bg = {32, 48, 64, 255};
    GXSetCopyClear(bg, 0xFFFFFF);
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
    GXCopyDisp(xfb, GX_TRUE);  // clears the EFB
    expectPixel("copy-clear background", 5, 5, 32, 48, 64);

    // 1. vertex colour through PASSCLR
    colorSetup();
    colorQuad(10, 10, 60, 60, 255, 0, 0, 255);
    expectPixel("vertex colour quad", 30, 30, 255, 0, 0);
    expectPixel("outside the quad", 70, 70, 32, 48, 64);

    // 2. textured quad with a two-stage TEV (texture*ras, then *konst)
    {
        u8 img[8 * 8 * 4];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                u8* p = img + (y * 8 + x) * 4;
                int q = (y >= 4) * 2 + (x >= 4);
                const u8 cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
                p[0] = cols[q][0]; p[1] = cols[q][1]; p[2] = cols[q][2]; p[3] = 255;
            }
        static std::vector<u8> tex;
        tex = encodeRGBA8(img, 8, 8);
        GXTexObj to;
        GXInitTexObj(&to, tex.data(), 8, 8, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&to, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&to, GX_TEXMAP0);
        texSetup();
        GXSetNumTevStages(2);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GXColor k = {255, 128, 64, 255};
        GXSetTevKColor(GX_KCOLOR0, k);
        GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
        GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        texQuad(100, 100, 300, 300, 128, 128, 128, 255);
        expectPixel("2-stage TEV, red texel", 150, 150, 128, 0, 0);
        expectPixel("2-stage TEV, green texel", 250, 150, 0, 64, 0);
        expectPixel("2-stage TEV, blue texel", 150, 250, 0, 0, 32);
        expectPixel("2-stage TEV, white texel", 250, 250, 128, 64, 32);

        // 3. EFB -> texture copy of that quad, drawn back with REPLACE
        u8* copyBuf = arena + 0x10000;
        GXSetTexCopySrc(100, 100, 200, 200);
        GXSetTexCopyDst(200, 200, GX_TF_RGBA8, GX_FALSE);
        GXCopyTex(copyBuf, GX_FALSE);
        GXTexObj co;
        GXInitTexObj(&co, copyBuf, 200, 200, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&co, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&co, GX_TEXMAP1);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        texQuad(400, 100, 500, 200, 255, 255, 255, 255);
        expectPixel("EFB copy sampled back (TL)", 425, 125, 128, 0, 0);
        expectPixel("EFB copy sampled back (BR)", 475, 175, 128, 64, 32);

        // 4. same copy through the I8 intensity path
        u8* copyI8 = arena + 0x40000;
        GXSetTexCopyDst(200, 200, GX_TF_I8, GX_FALSE);
        GXCopyTex(copyI8, GX_FALSE);
        GXInitTexObj(&co, copyI8, 200, 200, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&co, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&co, GX_TEXMAP1);
        texQuad(520, 100, 620, 200, 255, 255, 255, 255);
        // Y = 0.257*128 + 0.504*64 + 0.098*32 + 16 = 84.4
        expectPixel("I8 EFB copy (luma of white texel)", 595, 175, 84, 84, 84, 2);
    }

    // 5. blending and alpha compare
    colorSetup();
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    colorQuad(10, 10, 60, 60, 0, 0, 255, 128);
    expectPixel("alpha blend over red", 30, 30, 127, 0, 128, 3);
    GXSetAlphaCompare(GX_GREATER, 200, GX_AOP_AND, GX_ALWAYS, 0);
    colorQuad(10, 10, 60, 60, 0, 255, 0, 100);
    expectPixel("alpha compare rejects", 30, 30, 127, 0, 128, 3);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    colorQuad(10, 70, 60, 90, 16, 16, 16, 255);
    expectPixel("subtractive blend", 30, 80, 16, 32, 48, 1);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

    // 6. scissor
    GXSetScissor(0, 0, 20, 480);
    colorQuad(10, 100, 40, 120, 255, 255, 0, 255);
    expectPixel("inside scissor", 15, 110, 255, 255, 0);
    expectPixel("outside scissor", 30, 110, 32, 48, 64);
    GXSetScissor(0, 0, 640, 480);

    // 7. display list built with GD: a TEV register write and a draw command
    {
        static u8 dl[512] __attribute__((aligned(32)));
        GDLObj obj;
        GDInitGDLObj(&obj, dl, sizeof(dl));
        GDSetCurrent(&obj);
        GDSetCullMode(GX_CULL_NONE);
        GDSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GDWriteBPCmd((0xE2u << 24) | (0u) | (255u << 12));   // TEVREG0 R, A
        GDWriteBPCmd((0xE3u << 24) | (0u) | (200u << 12));   // TEVREG0 B, G
        GDBegin(GX_QUADS, GX_VTXFMT0, 4);
        GDPosition3f32(70, 10, -1); GDColor4u8(1, 2, 3, 255);
        GDPosition3f32(120, 10, -1); GDColor4u8(1, 2, 3, 255);
        GDPosition3f32(120, 60, -1); GDColor4u8(1, 2, 3, 255);
        GDPosition3f32(70, 60, -1); GDColor4u8(1, 2, 3, 255);
        GDEnd();
        GDPadCurr32();
        u32 size = u32(GDGetCurrOffset());
        GDSetCurrent(NULL);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
        GXCallDisplayList(dl, size);
        expectPixel("GD display list draw", 95, 35, 0, 200, 0);
    }

    // 8. write-gather pipe used directly (BP write of TEV register 0 = blue)
    {
        u32 ra = (0xE2u << 24) | (0u) | (255u << 12), bg = (0xE3u << 24) | 255u;
        GXPC_WGPipe.u8 = 0x61;
        GXPC_WGPipe.u32 = ra;
        GXPC_WGPipe.u8 = 0x61;
        GXPC_WGPipe.u32 = bg;
        colorQuad(130, 10, 180, 60, 9, 9, 9, 255);
        expectPixel("raw FIFO BP write", 155, 35, 0, 0, 255);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }

    // 9. perspective, depth test and a lit channel
    {
        Mtx44 p;
        memset(p, 0, sizeof(p));
        float n = 1, f = 100, cot = 1.0f / tanf(0.5f * 60.0f * 3.14159265f / 180.0f), aspect = 640.0f / 480.0f;
        p[0][0] = cot / aspect;
        p[1][1] = cot;
        p[2][2] = -n / (f - n);
        p[2][3] = -(f * n) / (f - n);
        p[3][2] = -1;
        GXSetProjection(p, GX_PERSPECTIVE);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        Mtx id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        GXLoadNrmMtxImm(id, GX_PNMTX0);
        GXLightObj lo;
        GXInitLightPos(&lo, 0, 0, 1000);
        GXColor white = {255, 255, 255, 255}, amb = {64, 64, 64, 255};
        GXInitLightColor(&lo, white);
        GXLoadLightObjImm(&lo, GX_LIGHT0);
        GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetChanAmbColor(GX_COLOR0A0, amb);
        GXColor mat = {200, 100, 50, 255};
        GXSetChanMatColor(GX_COLOR0A0, mat);
        auto quad = [](float cx, float cy, float z, float nz) {
            GXBegin(GX_QUADS, GX_VTXFMT2, 4);
            GXPosition3f32(cx - 1, cy + 1, z); GXNormal3f32(0, 0, nz);
            GXPosition3f32(cx + 1, cy + 1, z); GXNormal3f32(0, 0, nz);
            GXPosition3f32(cx + 1, cy - 1, z); GXNormal3f32(0, 0, nz);
            GXPosition3f32(cx - 1, cy - 1, z); GXNormal3f32(0, 0, nz);
            GXEnd();
        };
        // far quad drawn after the near one must lose the depth test
        quad(-2, -1.5f, -10, 1);   // lit: full material colour
        quad(-2, -1.5f, -20, -1);  // behind, facing away
        quad(2, -1.5f, -10, -1);   // unlit: ambient * material
        // screen position of (-2,-1.5,-10): x = 320 + 320 * (-2 * cot / aspect / 10)
        int lx = int(320 + 320 * (-2 * cot / aspect / 10)), ly = int(240 - 240 * (-1.5f * cot / 10));
        int rx = int(320 + 320 * (2 * cot / aspect / 10));
        expectPixel("lit quad (depth test keeps near)", lx, ly, 200, 100, 50, 2);
        // 64/255 * material
        expectPixel("unlit quad = ambient * material", rx, ly, 50, 25, 13, 2);
        u32 z = 0;
        GXPeekZ(u16(lx), u16(ly), &z);
        // depth of z=-10 with near 1 far 100: (f/(f-n)) - f*n/((f-n)*10) = 0.90909 * 16777215
        expect("GXPeekZ reports the near quad's depth", z > 15100000 && z < 15300000);
        // regression (file-select blocks were black): a spot-attenuated channel
        // whose second light has all-zero a/k coefficients must ignore that light
        // instead of producing 0/0 = NaN.
        GXInitLightAttn(&lo, 1, 0, 0, 1, 0, 0);
        GXInitLightDir(&lo, 0, 0, -1);
        GXLoadLightObjImm(&lo, GX_LIGHT0);
        GXLightObj zero;
        memset(&zero, 0, sizeof(zero));
        GXInitLightColor(&zero, white);
        GXInitLightPos(&zero, 0, 0, 1000);
        GXLoadLightObjImm(&zero, GX_LIGHT1);
        GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_SPOT);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        quad(0, 1.5f, -10, 1);
        // linear fog: eye distance 10 between start 0 and end 20 -> half fog colour
        GXColor fogc = {0, 0, 255, 255};
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetFog(GX_FOG_LIN, 0.0f, 20.0f, n, f, fogc);
        quad(2, 1.5f, -10, 1);
        GXSetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, fogc);
        {
            // the game's underwater fog: near 1, far 33333, start -8.4, end 111.6
            Mtx44 p2;
            memcpy(p2, p, sizeof(p2));
            float f2 = 33333.0f;
            p2[2][2] = -n / (f2 - n);
            p2[2][3] = -(f2 * n) / (f2 - n);
            GXSetProjection(p2, GX_PERSPECTIVE);
            GXSetFog(GX_FOG_LIN, -8.4f, 111.6f, n, f2, fogc);
            quad(-2, 1.5f, -10, 1);
            GXSetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, fogc);
            GXSetProjection(p, GX_PERSPECTIVE);
            int gx = int(320 + 320 * (-2 * cot / aspect / 10)), gy = int(240 - 240 * (1.5f * cot / 10));
            // (10 + 8.4) / 120 = 0.153 of the fog colour
            expectPixel("fog with a far plane of 33333", gx, gy, 169, 85, 81, 4);
        }
        int fx = int(320 + 320 * (2 * cot / aspect / 10)), fy = int(240 - 240 * (1.5f * cot / 10));
        expectPixel("linear fog at half distance", fx, fy, 100, 50, 153, 3);
        int cy = int(240 - 240 * (1.5f * cot / 10));
        expectPixel("spot light with zero attenuation ignored", 320, cy, 200, 100, 50, 2);
        setOrtho();
    }

    // 10. display copy and XFB readback
    GXCopyDisp(xfb, GX_FALSE);
    {
        int w = 0, h = 0;
        GXPC_ReadXFB(xfb, nullptr, &w, &h);
        std::vector<u8> px(size_t(w) * h * 4);
        GXPC_ReadXFB(xfb, px.data(), &w, &h);
        const u8* p = &px[(size_t(30) * w + 30) * 4];
        printf("     XFB %dx%d, pixel (30,30) = %d %d %d\n", w, h, p[0], p[1], p[2]);
        expect("XFB holds the displayed frame", w == 640 && h == 480 && near(p[0], 127, 3) && p[1] < 4 && near(p[2], 128, 3));
    }

    // 11. CPU texture decoders
    {
        u8 out[64 * 4];
        // I4 8x8 tile: first byte 0xF0 -> texel0 = 255, texel1 = 0
        u8 i4[32] = {0xF0, 0x7F};
        gx::decodeTexture(i4, GX_TF_I4, 8, 8, nullptr, 0, out);
        expect("I4 decode", out[0] == 255 && out[4] == 0 && out[8] == 7 * 17 && out[12] == 255);
        // RGB5A3: opaque 0xFC00 = red, translucent 0x4F00 = alpha 4/7, r=15
        u8 rgb5a3[32] = {0xFC, 0x00, 0x4F, 0x00};
        gx::decodeTexture(rgb5a3, GX_TF_RGB5A3, 4, 4, nullptr, 0, out);
        expect("RGB5A3 decode", out[0] == 255 && out[1] == 0 && out[3] == 255 && out[4] == 255 && out[7] == 4 * 255 / 7);
        // CMPR: c0 = white (0xFFFF) > c1 = black, index 1 -> black, 2 -> 2/3 white
        u8 cmpr[32] = {0xFF, 0xFF, 0x00, 0x00, 0x1B, 0x1B, 0x1B, 0x1B};
        gx::decodeTexture(cmpr, GX_TF_CMPR, 8, 8, nullptr, 0, out);
        expect("CMPR decode", out[0] == 255 && out[4] == 0 && out[8] == 170 && out[12] == 85);
        // C8 with an RGB565 TLUT
        u8 c8[32] = {0, 1};
        u8 tlut[4] = {0xF8, 0x00, 0x07, 0xE0};
        gx::decodeTexture(c8, GX_TF_C8, 8, 4, tlut, GX_TL_RGB565, out);
        expect("C8 + RGB565 TLUT decode", out[0] == 255 && out[1] == 0 && out[4] == 0 && out[5] == 255);
    }

    // Early-Z must retain an alpha-rejected surface's depth. Shadow volumes
    // use that depth to keep their alpha mask off foreground geometry (#33).
    {
        colorSetup();
        setOrtho();
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_FALSE, 0);
        GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        GXColor white = {255, 255, 255, 0};
        GXSetCopyClear(white, 0xFFFFFF);
        GXCopyDisp(xfb, GX_TRUE);
        for (int early = 0; early < 2; early++) {
            float x = 100.0f + early * 100.0f;
            GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
            colorQuad(x, 100, x + 80, 180, 255, 255, 255, 0, -3);
            GXSetZCompLoc(GXBool(early));
            GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
            GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
            colorQuad(x, 100, x + 80, 180, 255, 0, 0, 0, -2);
            expectPixel("alpha-rejected wall keeps colour", int(x + 40), 140, 255, 255, 255);
            u32 depth = 0;
            GXPeekZ(u16(x + 40), 140, &depth);
            expect(early ? "early alpha rejection writes depth" : "late alpha rejection preserves depth",
                   early ? depth > 3300000 && depth < 3400000 : depth > 5000000 && depth < 5100000);

            // Reset alpha, then the near and far faces of a shadow volume.
            GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            GXSetColorUpdate(GX_FALSE);
            GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
            GXSetDstAlpha(GX_TRUE, 0);
            colorQuad(x, 100, x + 80, 180, 0, 0, 0, 180, -2.5f);
            GXSetDstAlpha(GX_FALSE, 0);
            GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
            colorQuad(x, 100, x + 80, 180, 0, 0, 0, 180, -2.5f);
            GXSetColorUpdate(GX_TRUE);
            GXSetDstAlpha(GX_TRUE, 0);
            GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
            GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP);
            colorQuad(x, 100, x + 80, 180, 0, 0, 0, 180, -4);
            expectPixel(early ? "shadow blocked by early-Z wall" : "shadow visible without wall depth",
                        int(x + 40), 140, early ? 255 : 75, early ? 255 : 75, early ? 255 : 75);
            GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
            GXSetDstAlpha(GX_FALSE, 0);
        }
        // Both quads share one batch. The second must fail LESS against the
        // first even though its alpha passes; the fallback must keep order.
        GXSetZCompLoc(GX_TRUE);
        GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
        GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
        colorQuad(300, 100, 380, 180, 255, 0, 0, 255, -1);
        colorQuad(300, 100, 380, 180, 0, 0, 255, 255, -1);
        expectPixel("early-Z overlapping LESS keeps first", 340, 140, 255, 0, 0);
    }

    // Copy clears must preserve disabled channels and alpha in RGB8. The
    // airstrip boat shadow uses that backing alpha after switching to RGBA6.
    {
        colorSetup();
        setOrtho();
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_FALSE, 0);
        GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        colorQuad(400, 100, 480, 180, 200, 100, 50, 180, -3);
        u32 oldDepth = 0, depth = 0, pixel = 0;
        GXPeekZ(440, 140, &oldDepth);
        GXColor clear = {32, 48, 64, 255};
        GXSetCopyClear(clear, 0xFFFFFF);
        GXSetAlphaUpdate(GX_FALSE);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXCopyDisp(xfb, GX_TRUE);
        expectPixel("copy-clear enabled colour", 440, 140, 32, 48, 64);
        GXPeekARGB(440, 140, &pixel);
        expect("copy-clear preserves shadow alpha", (pixel >> 24) == 180);
        GXPeekZ(440, 140, &depth);
        expect("copy-clear preserves disabled depth", depth == oldDepth);

        GXSetAlphaUpdate(GX_TRUE);
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
        GXCopyDisp(xfb, GX_TRUE);
        GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        GXPeekARGB(440, 140, &pixel);
        expect("RGB8 copy-clear preserves backing alpha", (pixel >> 24) == 180);

        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        clear.r = 255;
        GXSetCopyClear(clear, 0xFFFFFF);
        GXCopyDisp(xfb, GX_TRUE);
        expectPixel("copy-clear preserves disabled colour", 440, 140, 32, 48, 64);
        GXPeekARGB(440, 140, &pixel);
        expect("copy-clear enabled alpha", (pixel >> 24) == 255);
        GXPeekZ(440, 140, &depth);
        expect("copy-clear enabled depth", depth == 0xFFFFFF);
        GXSetColorUpdate(GX_TRUE);
    }

    GXPCStats st;
    GXPC_GetStats(&st);
    printf("stats: %u draws, %u vertices, %u shaders compiled, %u texture uploads, %u EFB copies\n", st.draws, st.vertices,
           st.shaderCompiles, st.textureUploads, st.efbCopies);

    if (argc > 1) {
        int w, h;
        GXPC_ReadEFB(nullptr, &w, &h);
        std::vector<u8> px(size_t(w) * h * 4);
        GXPC_ReadEFB(px.data(), &w, &h);
        FILE* f = fopen(argv[1], "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) fwrite(&px[size_t(i) * 4], 1, 3, f);
            fclose(f);
        }
    }
    if (!GXPC_IsHeadless()) {
        GXPC_SetAutoPresent(1);
        for (int i = 0; i < 120; i++) GXPC_Present(xfb);
    }
    printf("%d passed, %d failed\n", s_pass, s_fail);
    GXPC_Shutdown();
    return s_fail ? 1 : 0;
}
