// The GX API: every call is translated into the BP/CP/XF register writes the
// hardware would receive.  Field layouts follow the decompiled SDK
// (decomp/libs/dolphin/src/gx) so that display lists built by GD/J3D and the API
// agree bit for bit.
#include "gx_internal.h"
#include "sms_gx/gx_pc.h"

#include <dolphin/gx.h>

#include <math.h>
#include <string.h>

#include "port_fpu.h"  // port_cvt_fp2unsigned: MWCC's __cvt_fp2unsigned, as the SDK converts
#include "gx_sdk_math.h"  // the SDK's float arithmetic, as the DOL computes it

using namespace gx;

namespace gx {
void traceNoteBegin();  // gx_trace.cpp (SMS_GX_TRACE_BT)
void traceNoteProjection(const float* p);
extern void (*drawSyncCallback)(uint16_t);
bool rendererReady();
}

// ------------------------------------------------------------------ helpers
static bool s_recording = false;
static uint8_t* s_dlBegin = nullptr;
static uint32_t s_dlSize = 0;

static inline uint32_t setField(uint32_t reg, int size, int shift, uint32_t v) {
    uint32_t m = ((1u << size) - 1u) << shift;
    return (reg & ~m) | ((v << shift) & m);
}
static inline uint32_t getField(uint32_t reg, int size, int shift) { return (reg >> shift) & ((1u << size) - 1u); }

static void apiBP(uint32_t reg, uint32_t data) {
    uint32_t v = (reg << 24) | (data & 0xFFFFFF);
    if (s_recording) {
        GXPC_Write8(0x61);
        GXPC_Write32(v);
        return;
    }
    writeBP(v);
}
static void bpField(uint32_t reg, int size, int shift, uint32_t v) { apiBP(reg, setField(g.bp[reg], size, shift, v)); }

static void apiCP(uint8_t reg, uint32_t v) {
    if (s_recording) {
        GXPC_Write8(0x08);
        GXPC_Write8(reg);
        GXPC_Write32(v);
        return;
    }
    writeCP(reg, v);
}

static void apiXF(uint16_t addr, uint32_t n, const uint32_t* w) {
    if (s_recording) {
        GXPC_Write8(0x10);
        GXPC_Write32(((n - 1) << 16) | addr);
        for (uint32_t i = 0; i < n; i++) GXPC_Write32(w[i]);
        return;
    }
    writeXF(addr, n, w);
}
static void apiXF1(uint16_t addr, uint32_t v) { apiXF(addr, 1, &v); }
static void apiXFf(uint16_t addr, uint32_t n, const float* f) {
    uint32_t w[16];
    memcpy(w, f, n * 4);
    apiXF(addr, n, w);
}
static inline uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static uint32_t xfReg(int r) { return g.xfReg[r]; }

// ------------------------------------------------------------------ internal object layouts
namespace {
struct TexObjInt {
    uint32_t mode0, mode1, image0, tlutName;
    uint8_t fmt, mipmap, isCI, pad;
    // 4-byte slots on 64-bit hosts (PTR32), so the object fits GXTexObj
    PTR32(const void) image;
    PTR32(void) user;
};
struct TlutObjInt {
    uint32_t fmt;
    PTR32(const void) data;
    uint16_t entries;
};
struct TlutRegionInt {
    uint32_t tmemAddr;
    uint32_t size;  // in 16-entry units
};
struct TexRegionInt {
    uint32_t tmemEven, tmemOdd;
    uint8_t is32b, cached;
};
struct LightObjInt {
    uint32_t reserved[3];
    uint32_t color;
    float a[3], k[3], pos[3], dir[3];
};
struct FifoObjInt {
    uint8_t* base;
    uint8_t* top;
    uint32_t size, hi, lo;
    uint8_t* rd;
    uint8_t* wr;
};
static_assert(sizeof(TexObjInt) <= sizeof(GXTexObj), "GXTexObj overlay too large");
static_assert(sizeof(TlutObjInt) <= sizeof(GXTlutObj), "GXTlutObj overlay too large");
static_assert(sizeof(TlutRegionInt) <= sizeof(GXTlutRegion), "GXTlutRegion overlay too large");
static_assert(sizeof(TexRegionInt) <= sizeof(GXTexRegion), "GXTexRegion overlay too large");
static_assert(sizeof(LightObjInt) <= sizeof(GXLightObj), "GXLightObj overlay too large");
static_assert(sizeof(FifoObjInt) <= sizeof(GXFifoObj), "GXFifoObj overlay too large");
}  // namespace

static GXFifoObj s_fifo;
static GXTlutRegion s_tlutRegions[20];
static TlutObjInt s_tlutLoaded[20];
static GXTexRegion s_texRegions[8];
static uint32_t s_nextTexRegion;
static GXTexRegionCallback s_texRegionCb;
static GXTlutRegionCallback s_tlutRegionCb;
static GXDrawDoneCallback s_drawDoneCb;
static GXBreakPtCallback s_breakPtCb;

// EFB copy shadows (the SDK keeps separate display and texture copy settings)
static uint32_t s_dispSrc, s_dispSize, s_dispStride, s_dispCtrl, s_dispYScale;
static uint32_t s_texSrc, s_texSize, s_texStride, s_texCtrl;
static bool s_texCopyZ;

static const uint8_t kFiltHw[6] = {0, 4, 1, 5, 2, 6};
static const uint8_t kFiltGx[8] = {GX_NEAR, GX_NEAR_MIP_NEAR, GX_NEAR_MIP_LIN, GX_NEAR, GX_LINEAR, GX_LIN_MIP_NEAR, GX_LIN_MIP_LIN, GX_LINEAR};

extern "C" {

// ================================================================== GXManage / GXInit
static GXTexRegion* defaultTexRegionCb(GXTexObj*, GXTexMapID) { return &s_texRegions[s_nextTexRegion++ & 7]; }
static GXTlutRegion* defaultTlutRegionCb(u32 idx) { return &s_tlutRegions[idx < 20 ? idx : 0]; }

GXFifoObj* GXInit(void* base, u32 size) {
    // bring up a window (or a headless context) unless the host already did
    if (!rendererReady()) GXPC_InitAuto(1);
    resetState();
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(&s_fifo);
    f->base = static_cast<uint8_t*>(base);
    f->top = f->base + size - 4;
    f->size = size;
    f->rd = f->wr = f->base;

    for (int i = 0; i < 16; i++) GXInitTlutRegion(&s_tlutRegions[i], 0xC0000 + i * 0x2000, GX_TLUT_256);
    for (int i = 0; i < 4; i++) GXInitTlutRegion(&s_tlutRegions[16 + i], 0xE0000 + i * 0x8000, GX_TLUT_1K);
    s_texRegionCb = defaultTexRegionCb;
    s_tlutRegionCb = defaultTlutRegionCb;

    // vertex formats: BYTEDEQUANT set in every VAT, as the SDK does
    for (int i = 0; i < 8; i++) {
        apiCP(uint8_t(CP_VAT_A + i), 1u << 30);
        apiCP(uint8_t(CP_VAT_B + i), 1u << 31);
        apiCP(uint8_t(CP_VAT_C + i), 0);
    }
    GXClearVtxDesc();
    GXSetCurrentMtx(GX_PNMTX0);
    for (int i = 0; i < 8; i++) GXSetTexCoordGen2(GXTexCoordID(i), GX_TG_MTX2x4, GXTexGenSrc(GX_TG_TEX0 + i), GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumTexGens(1);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXColor black = {0, 0, 0, 0}, white = {255, 255, 255, 255};
    GXSetChanAmbColor(GX_COLOR0A0, black);
    GXSetChanAmbColor(GX_COLOR1A1, black);
    GXSetChanMatColor(GX_COLOR0A0, white);
    GXSetChanMatColor(GX_COLOR1A1, white);
    GXSetNumTevStages(1);
    for (int i = 0; i < 16; i++) {
        GXSetTevOp(GXTevStageID(i), GX_REPLACE);
        GXSetTevOrder(GXTevStageID(i), i < 8 ? GXTexCoordID(i) : GX_TEXCOORD_NULL, i < 8 ? GXTexMapID(i) : GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevKColorSel(GXTevStageID(i), GX_TEV_KCSEL_1_4);
        GXSetTevKAlphaSel(GXTevStageID(i), GX_TEV_KASEL_1);
        GXSetTevSwapMode(GXTevStageID(i), GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect(GXTevStageID(i));
    }
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetNumIndStages(0);
    for (int i = 0; i < 4; i++) GXSetTevColor(GXTevRegID(i), white);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDither(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GXSetCullMode(GX_CULL_BACK);
    GXSetCoPlanar(GX_FALSE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetLineWidth(6, GX_TO_ZERO);
    GXSetPointSize(6, GX_TO_ZERO);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    GXSetScissorBoxOffset(0, 0);
    float ortho[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXColor fogc = {0, 0, 0, 0};
    GXSetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, fogc);
    GXSetFogRangeAdj(GX_FALSE, 0, nullptr);
    GXSetCopyClear(black, 0xFFFFFF);
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
    GXSetDispCopyYScale(1.0f);
    GXSetCopyClamp(GXFBClamp(GX_CLAMP_TOP | GX_CLAMP_BOTTOM));
    GXSetDispCopyGamma(GX_GM_1_0);
    GXSetDispCopyFrame2Field(GX_COPY_PROGRESSIVE);
    GXSetTexCopySrc(0, 0, 640, 480);
    GXSetTexCopyDst(640, 480, GX_TF_RGBA8, GX_FALSE);
    for (int i = 0; i < 8; i++) GXSetTexCoordScaleManually(GXTexCoordID(i), GX_FALSE, 0, 0);
    return &s_fifo;
}

void GXSetMisc(GXMiscToken, u32) {}
void GXFlush(void) { flushBatch(); }
void GXResetWriteGatherPipe(void) {}
void GXAbortFrame(void) {}
BOOL IsWriteGatherBufferEmpty(void) { return pipeIdle(); }

void GXSetDrawSync(u16 token) {
    apiBP(BP_PE_TOKEN_INT, 0x480000 | token);  // writes 0x48 with token
    apiBP(BP_PE_TOKEN, token);
}
u16 GXReadDrawSync(void) { return g.drawSyncToken; }
void GXSetDrawDone(void) {
    apiBP(BP_DRAWDONE, 2);
    if (s_drawDoneCb) s_drawDoneCb();
}
void GXWaitDrawDone(void) { flushBatch(); }
void GXDrawDone(void) {
    flushBatch();
    if (s_drawDoneCb) s_drawDoneCb();
}
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback old = reinterpret_cast<GXDrawSyncCallback>(drawSyncCallback);
    drawSyncCallback = reinterpret_cast<void (*)(uint16_t)>(cb);
    return old;
}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
    GXDrawDoneCallback old = s_drawDoneCb;
    s_drawDoneCb = cb;
    return old;
}

// ================================================================== GXFifo
void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(fifo);
    f->base = static_cast<uint8_t*>(base);
    f->top = f->base + size - 4;
    f->size = size;
    f->rd = f->wr = f->base;
}
void GXInitFifoPtrs(GXFifoObj* fifo, void* rd, void* wr) {
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(fifo);
    f->rd = static_cast<uint8_t*>(rd);
    f->wr = static_cast<uint8_t*>(wr);
}
void GXInitFifoLimits(GXFifoObj* fifo, u32 hi, u32 lo) {
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(fifo);
    f->hi = hi;
    f->lo = lo;
}
void GXSetCPUFifo(GXFifoObj*) {}
void GXSetGPFifo(GXFifoObj*) {}
void GXSaveCPUFifo(GXFifoObj*) {}
void GXSaveGPFifo(GXFifoObj*) {}
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    if (overhi) *overhi = 0;
    if (underlow) *underlow = 1;
    if (readIdle) *readIdle = 1;
    if (cmdIdle) *cmdIdle = 1;
    if (brkpt) *brkpt = 0;
}
void GXGetFifoStatus(GXFifoObj*, GXBool* overhi, GXBool* underflow, u32* count, GXBool* cpuWrite, GXBool* gpRead, GXBool* wrap) {
    if (overhi) *overhi = 0;
    if (underflow) *underflow = 1;
    if (count) *count = 0;
    if (cpuWrite) *cpuWrite = 1;
    if (gpRead) *gpRead = 1;
    if (wrap) *wrap = 0;
}
void GXGetFifoPtrs(GXFifoObj* fifo, void** rd, void** wr) {
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(fifo);
    if (rd) *rd = f->rd;
    if (wr) *wr = f->wr;
}
void* GXGetFifoBase(GXFifoObj* fifo) { return reinterpret_cast<FifoObjInt*>(fifo)->base; }
u32 GXGetFifoSize(GXFifoObj* fifo) { return reinterpret_cast<FifoObjInt*>(fifo)->size; }
void GXGetFifoLimits(GXFifoObj* fifo, u32* hi, u32* lo) {
    FifoObjInt* f = reinterpret_cast<FifoObjInt*>(fifo);
    if (hi) *hi = f->hi;
    if (lo) *lo = f->lo;
}
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) {
    GXBreakPtCallback old = s_breakPtCb;
    s_breakPtCb = cb;
    return old;
}
void GXEnableBreakPt(void*) {}
void GXDisableBreakPt(void) {}
OSThread* GXSetCurrentGXThread(void) { return nullptr; }
OSThread* GXGetCurrentGXThread(void) { return nullptr; }
GXFifoObj* GXGetCPUFifo(void) { return &s_fifo; }
GXFifoObj* GXGetGPFifo(void) { return &s_fifo; }
u32 GXGetOverflowCount(void) { return 0; }
u32 GXResetOverflowCount(void) { return 0; }
volatile void* GXRedirectWriteGatherPipe(void* ptr) {
    setPipeRedirect(static_cast<uint8_t*>(ptr));
    return reinterpret_cast<volatile void*>(static_cast<uintptr_t>(0xCC008000u));
}
void GXRestoreWriteGatherPipe(void) { setPipeRedirect(nullptr); }

// ================================================================== display lists
void GXBeginDisplayList(void* list, u32 size) {
    flushBatch();
    s_recording = true;
    s_dlBegin = static_cast<uint8_t*>(list);
    s_dlSize = size;
    setPipeRedirect(s_dlBegin);
}
u32 GXEndDisplayList(void) {
    uint8_t* end = getPipeRedirect();
    setPipeRedirect(nullptr);
    s_recording = false;
    uint32_t n = uint32_t(end - s_dlBegin);
    while (n & 31) s_dlBegin[n++] = 0;  // pad with NOPs to 32 bytes
    if (n > s_dlSize) logmsg("display list overflow (%u > %u)", n, s_dlSize);
    return n;
}
void GXCallDisplayList(void* list, u32 nbytes) {
    if (s_recording) {
        GXPC_Write8(0x40);
        GXPC_Write32(ptrToPhys(list));
        GXPC_Write32(uint32_t(nbytes));
        return;
    }
    runCommands(static_cast<const uint8_t*>(list), uint32_t(nbytes));
}

// ================================================================== GXGeometry / GXAttr
void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    uint32_t lo = g.cpVcdLo, hi = g.cpVcdHi;
    uint32_t t = uint32_t(type) & 3;
    if (attr == GX_VA_PNMTXIDX) lo = setField(lo, 1, 0, t != 0);
    else if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) lo = setField(lo, 1, 1 + (attr - GX_VA_TEX0MTXIDX), t != 0);
    else if (attr == GX_VA_POS) lo = setField(lo, 2, 9, t);
    else if (attr == GX_VA_NRM || attr == GX_VA_NBT) lo = setField(lo, 2, 11, t);
    else if (attr == GX_VA_CLR0) lo = setField(lo, 2, 13, t);
    else if (attr == GX_VA_CLR1) lo = setField(lo, 2, 15, t);
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) hi = setField(hi, 2, 2 * (attr - GX_VA_TEX0), t);
    if (lo != g.cpVcdLo) apiCP(CP_VCD_LO, lo);
    if (hi != g.cpVcdHi) apiCP(CP_VCD_HI, hi);
}
void GXSetVtxDescv(const GXVtxDescList* list) {
    for (; list->attr != GX_VA_NULL; list++) GXSetVtxDesc(list->attr, list->type);
}
void GXClearVtxDesc(void) {
    apiCP(CP_VCD_LO, 1u << 9);  // position direct, as the SDK leaves it
    apiCP(CP_VCD_HI, 0);
}
void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
    uint32_t lo = g.cpVcdLo, hi = g.cpVcdHi, t = 0;
    if (attr == GX_VA_PNMTXIDX) t = lo & 1;
    else if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) t = getField(lo, 1, 1 + attr - GX_VA_TEX0MTXIDX);
    else if (attr == GX_VA_POS) t = getField(lo, 2, 9);
    else if (attr == GX_VA_NRM || attr == GX_VA_NBT) t = getField(lo, 2, 11);
    else if (attr == GX_VA_CLR0) t = getField(lo, 2, 13);
    else if (attr == GX_VA_CLR1) t = getField(lo, 2, 15);
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) t = getField(hi, 2, 2 * (attr - GX_VA_TEX0));
    *type = GXAttrType(t);
}
void GXGetVtxDescv(GXVtxDescList* vcd) {
    int n = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        vcd[n].attr = GXAttr(a);
        GXGetVtxDesc(GXAttr(a), &vcd[n].type);
        n++;
    }
    vcd[n].attr = GX_VA_NULL;
}

void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    uint32_t A = g.cpVatA[fmt], B = g.cpVatB[fmt], C = g.cpVatC[fmt];
    uint32_t c = uint32_t(cnt), t = uint32_t(type), f = frac;
    switch (attr) {
    case GX_VA_POS: A = setField(A, 1, 0, c); A = setField(A, 3, 1, t); A = setField(A, 5, 4, f); break;
    case GX_VA_NRM:
    case GX_VA_NBT:
        A = setField(A, 3, 10, t);
        if (c == GX_NRM_NBT3) { A = setField(A, 1, 9, 1); A = setField(A, 1, 31, 1); }
        else { A = setField(A, 1, 9, c); A = setField(A, 1, 31, 0); }
        break;
    case GX_VA_CLR0: A = setField(A, 1, 13, c); A = setField(A, 3, 14, t); break;
    case GX_VA_CLR1: A = setField(A, 1, 17, c); A = setField(A, 3, 18, t); break;
    case GX_VA_TEX0: A = setField(A, 1, 21, c); A = setField(A, 3, 22, t); A = setField(A, 5, 25, f); break;
    case GX_VA_TEX1: B = setField(B, 1, 0, c); B = setField(B, 3, 1, t); B = setField(B, 5, 4, f); break;
    case GX_VA_TEX2: B = setField(B, 1, 9, c); B = setField(B, 3, 10, t); B = setField(B, 5, 13, f); break;
    case GX_VA_TEX3: B = setField(B, 1, 18, c); B = setField(B, 3, 19, t); B = setField(B, 5, 22, f); break;
    case GX_VA_TEX4: B = setField(B, 1, 27, c); B = setField(B, 3, 28, t); C = setField(C, 5, 0, f); break;
    case GX_VA_TEX5: C = setField(C, 1, 5, c); C = setField(C, 3, 6, t); C = setField(C, 5, 9, f); break;
    case GX_VA_TEX6: C = setField(C, 1, 14, c); C = setField(C, 3, 15, t); C = setField(C, 5, 18, f); break;
    case GX_VA_TEX7: C = setField(C, 1, 23, c); C = setField(C, 3, 24, t); C = setField(C, 5, 27, f); break;
    default: break;
    }
    A |= 1u << 30;
    if (A != g.cpVatA[fmt]) apiCP(uint8_t(CP_VAT_A + fmt), A);
    if (B != g.cpVatB[fmt]) apiCP(uint8_t(CP_VAT_B + fmt), B);
    if (C != g.cpVatC[fmt]) apiCP(uint8_t(CP_VAT_C + fmt), C);
}
void GXSetVtxAttrFmtv(GXVtxFmt fmt, const GXVtxAttrFmtList* list) {
    for (; list->attr != GX_VA_NULL; list++) GXSetVtxAttrFmt(fmt, list->attr, list->cnt, list->type, list->frac);
}
void GXGetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt* cnt, GXCompType* type, u8* frac) {
    uint32_t A = g.cpVatA[fmt], B = g.cpVatB[fmt], C = g.cpVatC[fmt];
    uint32_t c = 0, t = 0, f = 0;
    switch (attr) {
    case GX_VA_POS: c = getField(A, 1, 0); t = getField(A, 3, 1); f = getField(A, 5, 4); break;
    case GX_VA_NRM:
    case GX_VA_NBT: c = getField(A, 1, 9); t = getField(A, 3, 10); if (c && (A >> 31)) c = GX_NRM_NBT3; break;
    case GX_VA_CLR0: c = getField(A, 1, 13); t = getField(A, 3, 14); break;
    case GX_VA_CLR1: c = getField(A, 1, 17); t = getField(A, 3, 18); break;
    case GX_VA_TEX0: c = getField(A, 1, 21); t = getField(A, 3, 22); f = getField(A, 5, 25); break;
    case GX_VA_TEX1: c = getField(B, 1, 0); t = getField(B, 3, 1); f = getField(B, 5, 4); break;
    case GX_VA_TEX2: c = getField(B, 1, 9); t = getField(B, 3, 10); f = getField(B, 5, 13); break;
    case GX_VA_TEX3: c = getField(B, 1, 18); t = getField(B, 3, 19); f = getField(B, 5, 22); break;
    case GX_VA_TEX4: c = getField(B, 1, 27); t = getField(B, 3, 28); f = getField(C, 5, 0); break;
    case GX_VA_TEX5: c = getField(C, 1, 5); t = getField(C, 3, 6); f = getField(C, 5, 9); break;
    case GX_VA_TEX6: c = getField(C, 1, 14); t = getField(C, 3, 15); f = getField(C, 5, 18); break;
    case GX_VA_TEX7: c = getField(C, 1, 23); t = getField(C, 3, 24); f = getField(C, 5, 27); break;
    default: break;
    }
    *cnt = GXCompCnt(c);
    *type = GXCompType(t);
    *frac = u8(f);
}
void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* vat) {
    int n = 0;
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        vat[n].attr = GXAttr(a);
        GXGetVtxAttrFmt(fmt, GXAttr(a), &vat[n].cnt, &vat[n].type, &vat[n].frac);
        n++;
    }
    vat[n].attr = GX_VA_NULL;
}

void GXSetArray(GXAttr attr, const void* base, u8 stride) {
    int slot = attr == GX_VA_NBT ? 1 : int(attr) - GX_VA_POS;
    if (slot < 0 || slot > 15) return;
    if (s_recording) {
        apiCP(uint8_t(CP_ARRAY_BASE + slot), ptrToPhys(base));
        apiCP(uint8_t(CP_ARRAY_STRIDE + slot), stride);
        return;
    }
    g.arrayBase[slot] = static_cast<const uint8_t*>(base);
    g.arrayStride[slot] = stride;
    g.arrayBigEndian[slot] = g_defaultArrayBE || isBigEndianData(base);
    g_arrayGen++;
}
void GXInvalidateVtxCache(void) {}

void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 pt) {
    if (dst >= GX_MAX_TEXCOORD) return;
    uint32_t reg = 0, row = 5, form = 0;
    switch (src) {
    case GX_TG_POS: row = 0; form = 1; break;
    case GX_TG_NRM: row = 1; form = 1; break;
    case GX_TG_BINRM: row = 3; form = 1; break;
    case GX_TG_TANGENT: row = 4; form = 1; break;
    case GX_TG_COLOR0:
    case GX_TG_COLOR1: row = 2; break;
    default:
        if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) row = 5 + (src - GX_TG_TEX0);
        else if (src >= GX_TG_TEXCOORD0 && src <= GX_TG_TEXCOORD6) row = 5 + (src - GX_TG_TEXCOORD0);
        break;
    }
    if (func == GX_TG_MTX2x4 || func == GX_TG_MTX3x4) {
        reg = setField(reg, 1, 1, func == GX_TG_MTX3x4);
        reg = setField(reg, 1, 2, form);
        reg = setField(reg, 3, 4, 0);
        reg = setField(reg, 5, 7, row);
    } else if (func >= GX_TG_BUMP0 && func <= GX_TG_BUMP7) {
        reg = setField(reg, 1, 2, form);
        reg = setField(reg, 3, 4, 1);
        reg = setField(reg, 5, 7, row);
        reg = setField(reg, 3, 12, src >= GX_TG_TEXCOORD0 ? src - GX_TG_TEXCOORD0 : 0);
        reg = setField(reg, 3, 15, func - GX_TG_BUMP0);
    } else {  // GX_TG_SRTG
        reg = setField(reg, 1, 2, form);
        reg = setField(reg, 3, 4, src == GX_TG_COLOR1 ? 3 : 2);
        reg = setField(reg, 5, 7, 2);
    }
    apiXF1(uint16_t(XF_REG_BASE + XFR_TEXGEN + dst), reg);
    uint32_t post = setField(0, 6, 0, pt - GX_PTTEXMTX0);
    post = setField(post, 1, 8, normalize);
    apiXF1(uint16_t(XF_REG_BASE + XFR_POSTTEX + dst), post);
    // texture matrix index for this coordinate
    if (dst < 4) {
        uint32_t a = setField(xfReg(XFR_MATIDX_A), 6, 6 + 6 * dst, mtx);
        apiXF1(XF_REG_BASE + XFR_MATIDX_A, a);
        apiCP(CP_MATIDX_A, a);
    } else {
        uint32_t b = setField(xfReg(XFR_MATIDX_B), 6, 6 * (dst - 4), mtx);
        apiXF1(XF_REG_BASE + XFR_MATIDX_B, b);
        apiCP(CP_MATIDX_B, b);
    }
}
void GXSetNumTexGens(u8 n) {
    bpField(BP_GENMODE, 4, 0, n);
    apiXF1(XF_REG_BASE + XFR_NUMTEXGENS, n);
}

GXBool __GXinBegin;  // read by GXEnd when GXGeometry.h is built with DEBUG

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 nverts) {
    __GXinBegin = GX_TRUE;
    traceNoteBegin();
    GXPC_Write8(uint8_t(type | fmt));
    GXPC_Write16(nverts);
}

void GXSetLineWidth(u8 width, GXTexOffset off) {
    g.lineWidth = width;
    uint32_t r = setField(g.bp[BP_LPSIZE], 8, 0, width);
    apiBP(BP_LPSIZE, setField(r, 3, 16, off));
}
void GXGetLineWidth(u8* width, GXTexOffset* off) {
    *width = u8(getField(g.bp[BP_LPSIZE], 8, 0));
    *off = GXTexOffset(getField(g.bp[BP_LPSIZE], 3, 16));
}
void GXSetPointSize(u8 size, GXTexOffset off) {
    g.pointSize = size;
    uint32_t r = setField(g.bp[BP_LPSIZE], 8, 8, size);
    apiBP(BP_LPSIZE, setField(r, 3, 19, off));
}
void GXGetPointSize(u8* size, GXTexOffset* off) {
    *size = u8(getField(g.bp[BP_LPSIZE], 8, 8));
    *off = GXTexOffset(getField(g.bp[BP_LPSIZE], 3, 19));
}
void GXEnableTexOffsets(GXTexCoordID coord, u8 line, u8 point) {
    uint32_t r = setField(g.bp[BP_SU_SSIZE + 2 * coord], 1, 18, line);
    apiBP(BP_SU_SSIZE + 2 * coord, setField(r, 1, 19, point));
}
void GXSetCullMode(GXCullMode mode) {
    g.cullMode = mode;
    uint32_t hw = mode == GX_CULL_FRONT ? 2 : mode == GX_CULL_BACK ? 1 : uint32_t(mode);
    bpField(BP_GENMODE, 2, 14, hw);
}
void GXGetCullMode(GXCullMode* mode) { *mode = GXCullMode(g.cullMode); }
void GXSetCoPlanar(GXBool enable) { bpField(BP_GENMODE, 1, 19, enable); }

// ================================================================== GXTransform
void GXSetProjection(f32 mtx[4][4], GXProjectionType type) {
    float p[7];
    p[0] = float(type);
    p[1] = mtx[0][0];
    p[3] = mtx[1][1];
    p[5] = mtx[2][2];
    p[6] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        p[2] = mtx[0][3];
        p[4] = mtx[1][3];
    } else {
        p[2] = mtx[0][2];
        p[4] = mtx[1][2];
    }
    GXSetProjectionv(p);
}
void GXSetProjectionv(f32* ptr) {
    traceNoteProjection(ptr);
    memcpy(g.projection, ptr, sizeof(g.projection));
    uint32_t w[7];
    for (int i = 0; i < 6; i++) w[i] = fbits(ptr[1 + i]);
    w[6] = ptr[0] != 0.0f ? 1 : 0;
    apiXF(XF_REG_BASE + XFR_PROJ, 7, w);
}
void GXGetProjectionv(f32* ptr) { memcpy(ptr, g.projection, sizeof(g.projection)); }

void GXLoadPosMtxImm(f32 mtx[3][4], u32 id) { apiXFf(uint16_t(id * 4), 12, &mtx[0][0]); }
void GXLoadPosMtxIndx(u16 idx, u32 id) {
    GXPC_Write8(0x20);
    GXPC_Write16(idx);
    GXPC_Write16(uint16_t((11u << 12) | (id * 4)));
}
void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id) {
    float m[9] = {mtx[0][0], mtx[0][1], mtx[0][2], mtx[1][0], mtx[1][1], mtx[1][2], mtx[2][0], mtx[2][1], mtx[2][2]};
    apiXFf(uint16_t(0x400 + id * 3), 9, m);
}
void GXLoadNrmMtxImm3x3(f32 mtx[3][3], u32 id) { apiXFf(uint16_t(0x400 + id * 3), 9, &mtx[0][0]); }
void GXLoadNrmMtxIndx3x3(u16 idx, u32 id) {
    GXPC_Write8(0x28);
    GXPC_Write16(idx);
    GXPC_Write16(uint16_t((8u << 12) | (0x400 + id * 3)));
}
void GXSetCurrentMtx(u32 id) {
    uint32_t a = setField(xfReg(XFR_MATIDX_A), 6, 0, id);
    apiXF1(XF_REG_BASE + XFR_MATIDX_A, a);
    apiCP(CP_MATIDX_A, a);
}
void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type) {
    uint16_t addr;
    uint32_t n;
    if (id >= GX_PTTEXMTX0) {
        addr = uint16_t(0x500 + (id - GX_PTTEXMTX0) * 4);
        n = 12;
    } else {
        addr = uint16_t(id * 4);
        n = type == GX_MTX2x4 ? 8 : 12;
    }
    apiXFf(addr, n, &mtx[0][0]);
}
void GXLoadTexMtxIndx(u16 idx, u32 id, GXTexMtxType type) {
    uint32_t addr, n;
    if (id >= GX_PTTEXMTX0) {
        addr = 0x500 + (id - GX_PTTEXMTX0) * 4;
        n = 12;
    } else {
        addr = id * 4;
        n = type == GX_MTX2x4 ? 8 : 12;
    }
    GXPC_Write8(0x30);
    GXPC_Write16(idx);
    GXPC_Write16(uint16_t(((n - 1) << 12) | addr));
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    if (field == 0) top -= 0.5f;
    g.viewport[0] = left;
    g.viewport[1] = top;
    g.viewport[2] = wd;
    g.viewport[3] = ht;
    g.viewport[4] = nearz;
    g.viewport[5] = farz;
    float v[6];
    v[0] = wd * 0.5f;
    v[1] = -ht * 0.5f;
    v[2] = (farz - nearz) * 16777215.0f;
    v[3] = left + wd * 0.5f + 342.0f;
    v[4] = top + ht * 0.5f + 342.0f;
    v[5] = farz * 16777215.0f;
    apiXFf(XF_REG_BASE + XFR_VIEWPORT, 6, v);
}
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1);
}
void GXGetViewportv(f32* vp) { memcpy(vp, g.viewport, sizeof(g.viewport)); }

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    g.scissor[0] = uint32_t(left);
    g.scissor[1] = uint32_t(top);
    g.scissor[2] = uint32_t(wd);
    g.scissor[3] = uint32_t(ht);
    uint32_t tl = setField(0, 11, 0, uint32_t(top) + 342);
    tl = setField(tl, 11, 12, uint32_t(left) + 342);
    uint32_t br = setField(0, 11, 0, uint32_t(top + ht) + 341);
    br = setField(br, 11, 12, uint32_t(left + wd) + 341);
    apiBP(BP_SCISSOR_TL, tl);
    apiBP(BP_SCISSOR_BR, br);
}
void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    *left = g.scissor[0];
    *top = g.scissor[1];
    *wd = g.scissor[2];
    *ht = g.scissor[3];
}
void GXSetScissorBoxOffset(s32 x, s32 y) {
    uint32_t r = setField(0, 10, 0, uint32_t(x + 342) >> 1);
    apiBP(BP_SCISSOR_OFS, setField(r, 10, 10, uint32_t(y + 342) >> 1));
}
void GXSetClipMode(GXClipMode mode) { apiXF1(XF_REG_BASE + 0x05, uint32_t(mode)); }

void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp, f32* sx, f32* sy, f32* sz) {
    gxsdkProject(x, y, z, mtx, pm, vp, sx, sy, sz);
}

// ================================================================== GXLight
void GXInitLightAttn(GXLightObj* o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}
void GXInitLightAttnA(GXLightObj* o, f32 a0, f32 a1, f32 a2) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
}
void GXInitLightAttnK(GXLightObj* o, f32 k0, f32 k1, f32 k2) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}
// Not in the DOL (UNUSED in marioUS.MAP) and never called by the game, so it
// keeps the host's cosf; the SDK's calls MSL's (gx_sdk_math.h).
void GXInitLightSpot(GXLightObj* o, f32 cutoff, GXSpotFn fn) {
    float a0, a1, a2;
    if (cutoff <= 0.0f || cutoff > 90.0f) fn = GX_SP_OFF;
    float cr = cosf(cutoff * 3.14159265f / 180.0f);
    switch (fn) {
    case GX_SP_FLAT: a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;
    case GX_SP_COS: a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break;
    case GX_SP_COS2: a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break;
    case GX_SP_SHARP: {
        float d = (1.0f - cr) * (1.0f - cr);
        a0 = cr * (cr - 2.0f) / d; a1 = 2.0f / d; a2 = -1.0f / d;
        break;
    }
    case GX_SP_RING1: {
        float d = (1.0f - cr) * (1.0f - cr);
        a0 = -4.0f * cr / d; a1 = 4.0f * (1.0f + cr) / d; a2 = -4.0f / d;
        break;
    }
    case GX_SP_RING2: {
        float d = (1.0f - cr) * (1.0f - cr);
        a0 = 1.0f - 2.0f * cr * cr / d; a1 = 4.0f * cr / d; a2 = -2.0f / d;
        break;
    }
    default: a0 = 1.0f; a1 = 0.0f; a2 = 0.0f; break;
    }
    GXInitLightAttnA(o, a0, a1, a2);
}
void GXInitLightDistAttn(GXLightObj* o, f32 refDist, f32 refBr, GXDistAttnFn fn) {
    float k[3];
    gxsdkLightDistAttn(refDist, refBr, int(fn), k);
    GXInitLightAttnK(o, k[0], k[1], k[2]);
}
void GXInitLightPos(GXLightObj* o, f32 x, f32 y, f32 z) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
}
void GXInitLightDir(GXLightObj* o, f32 nx, f32 ny, f32 nz) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->dir[0] = -nx; l->dir[1] = -ny; l->dir[2] = -nz;
}
void GXInitSpecularDir(GXLightObj* o, f32 nx, f32 ny, f32 nz) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    gxsdkSpecularDir(nx, ny, nz, l->dir, l->pos);
}
void GXInitSpecularDirHA(GXLightObj* o, f32 nx, f32 ny, f32 nz, f32 hx, f32 hy, f32 hz) {
    LightObjInt* l = reinterpret_cast<LightObjInt*>(o);
    l->dir[0] = hx; l->dir[1] = hy; l->dir[2] = hz;
    l->pos[0] = -nx * 1048576.0f; l->pos[1] = -ny * 1048576.0f; l->pos[2] = -nz * 1048576.0f;
}
void GXInitLightColor(GXLightObj* o, GXColor c) {
    reinterpret_cast<LightObjInt*>(o)->color = uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8 | c.a;
}
static int lightIndex(GXLightID light) {
    for (int i = 0; i < 8; i++)
        if (uint32_t(light) == (1u << i)) return i;
    return 0;
}
void GXLoadLightObjImm(GXLightObj* o, GXLightID light) {
    const LightObjInt* l = reinterpret_cast<const LightObjInt*>(o);
    uint32_t w[16] = {0, 0, 0, l->color};
    for (int i = 0; i < 3; i++) {
        w[4 + i] = fbits(l->a[i]);
        w[7 + i] = fbits(l->k[i]);
        w[10 + i] = fbits(l->pos[i]);
        w[13 + i] = fbits(l->dir[i]);
    }
    apiXF(uint16_t(0x600 + lightIndex(light) * 16), 16, w);
}
void GXLoadLightObjIndx(u32 idx, GXLightID light) {
    GXPC_Write8(0x38);
    GXPC_Write16(uint16_t(idx));
    GXPC_Write16(uint16_t((15u << 12) | (0x600 + lightIndex(light) * 16)));
}
void GXGetLightAttnA(const GXLightObj* o, f32* a0, f32* a1, f32* a2) {
    const LightObjInt* l = reinterpret_cast<const LightObjInt*>(o);
    *a0 = l->a[0]; *a1 = l->a[1]; *a2 = l->a[2];
}
void GXGetLightAttnK(const GXLightObj* o, f32* k0, f32* k1, f32* k2) {
    const LightObjInt* l = reinterpret_cast<const LightObjInt*>(o);
    *k0 = l->k[0]; *k1 = l->k[1]; *k2 = l->k[2];
}
void GXGetLightPos(const GXLightObj* o, f32* x, f32* y, f32* z) {
    const LightObjInt* l = reinterpret_cast<const LightObjInt*>(o);
    *x = l->pos[0]; *y = l->pos[1]; *z = l->pos[2];
}
void GXGetLightDir(const GXLightObj* o, f32* x, f32* y, f32* z) {
    const LightObjInt* l = reinterpret_cast<const LightObjInt*>(o);
    *x = -l->dir[0]; *y = -l->dir[1]; *z = -l->dir[2];
}
void GXGetLightColor(const GXLightObj* o, GXColor* c) {
    uint32_t v = reinterpret_cast<const LightObjInt*>(o)->color;
    c->r = u8(v >> 24); c->g = u8(v >> 16); c->b = u8(v >> 8); c->a = u8(v);
}

static void setChanColor(int base, GXChannelID chan, GXColor c) {
    int idx = (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ? 1 : 0;
    uint32_t cur = xfReg(base + idx);
    uint32_t rgb = uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8;
    uint32_t v;
    if (chan == GX_COLOR0 || chan == GX_COLOR1) v = rgb | (cur & 0xFF);
    else if (chan == GX_ALPHA0 || chan == GX_ALPHA1) v = (cur & 0xFFFFFF00u) | c.a;
    else v = rgb | c.a;
    apiXF1(uint16_t(XF_REG_BASE + base + idx), v);
}
void GXSetChanAmbColor(GXChannelID chan, GXColor c) { setChanColor(XFR_AMB0, chan, c); }
void GXSetChanMatColor(GXChannelID chan, GXColor c) { setChanColor(XFR_MAT0, chan, c); }
void GXSetNumChans(u8 n) {
    bpField(BP_GENMODE, 3, 4, n);
    apiXF1(XF_REG_BASE + XFR_NUMCHANS, n);
}
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb, GXColorSrc mat, u32 mask, GXDiffuseFn diff, GXAttnFn attn) {
    uint32_t idx = chan == GX_COLOR0A0 ? 0 : chan == GX_COLOR1A1 ? 1 : uint32_t(chan);
    uint32_t r = 0;
    r = setField(r, 1, 1, enable);
    r = setField(r, 1, 0, mat);
    r = setField(r, 1, 6, amb);
    r = setField(r, 4, 2, uint32_t(mask) & 15);
    r = setField(r, 4, 11, (uint32_t(mask) >> 4) & 15);
    r = setField(r, 2, 7, attn == GX_AF_SPEC ? 0 : uint32_t(diff));
    r = setField(r, 1, 9, attn != GX_AF_NONE);
    r = setField(r, 1, 10, attn != GX_AF_SPEC);
    apiXF1(uint16_t(XF_REG_BASE + XFR_COLOR0CTRL + idx), r);
    if (chan == GX_COLOR0A0) apiXF1(XF_REG_BASE + XFR_ALPHA0CTRL, r);
    else if (chan == GX_COLOR1A1) apiXF1(XF_REG_BASE + XFR_ALPHA0CTRL + 1, r);
}

// ================================================================== GXTev
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GXTevColorArg carg = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    GXTevAlphaArg aarg = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(id, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    case GX_BLEND:
        GXSetTevColorIn(id, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    default:  // GX_PASSCLR
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    uint32_t r = g.bp[BP_TEV_COLOR_ENV + 2 * s];
    r = setField(r, 4, 12, a);
    r = setField(r, 4, 8, b);
    r = setField(r, 4, 4, c);
    r = setField(r, 4, 0, d);
    apiBP(BP_TEV_COLOR_ENV + 2 * s, r);
}
void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    uint32_t r = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1];
    r = setField(r, 3, 13, a);
    r = setField(r, 3, 10, b);
    r = setField(r, 3, 7, c);
    r = setField(r, 3, 4, d);
    apiBP(BP_TEV_COLOR_ENV + 2 * s + 1, r);
}
static uint32_t tevOp(uint32_t r, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    r = setField(r, 1, 18, op & 1);
    if (op <= GX_TEV_SUB) {
        r = setField(r, 2, 20, scale);
        r = setField(r, 2, 16, bias);
    } else {
        r = setField(r, 2, 20, (op >> 1) & 3);
        r = setField(r, 2, 16, 3);
    }
    r = setField(r, 1, 19, clamp);
    return setField(r, 2, 22, out);
}
void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    apiBP(BP_TEV_COLOR_ENV + 2 * s, tevOp(g.bp[BP_TEV_COLOR_ENV + 2 * s], op, bias, scale, clamp, out));
}
void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    apiBP(BP_TEV_COLOR_ENV + 2 * s + 1, tevOp(g.bp[BP_TEV_COLOR_ENV + 2 * s + 1], op, bias, scale, clamp, out));
}
static void tevRegWrite(uint32_t id, int r, int gg, int b, int a, bool konst) {
    uint32_t ra = setField(0, 11, 0, uint32_t(r)) | setField(0, 11, 12, uint32_t(a)) | (konst ? 0x800000u : 0);
    uint32_t bg = setField(0, 11, 0, uint32_t(b)) | setField(0, 11, 12, uint32_t(gg)) | (konst ? 0x800000u : 0);
    apiBP(BP_TEV_REG + 2 * id, ra);
    apiBP(BP_TEV_REG + 2 * id + 1, bg);
}
void GXSetTevColor(GXTevRegID id, GXColor c) { tevRegWrite(id, c.r, c.g, c.b, c.a, false); }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 c) { tevRegWrite(id, c.r, c.g, c.b, c.a, false); }
void GXSetTevKColor(GXTevKColorID id, GXColor c) { tevRegWrite(id, c.r, c.g, c.b, c.a, true); }
void GXSetTevKColorSel(GXTevStageID s, GXTevKColorSel sel) {
    bpField(BP_TEV_KSEL + (s >> 1), 5, (s & 1) ? 14 : 4, sel);
}
void GXSetTevKAlphaSel(GXTevStageID s, GXTevKAlphaSel sel) {
    bpField(BP_TEV_KSEL + (s >> 1), 5, (s & 1) ? 19 : 9, sel);
}
void GXSetTevSwapMode(GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex) {
    uint32_t r = setField(g.bp[BP_TEV_COLOR_ENV + 2 * s + 1], 2, 0, ras);
    apiBP(BP_TEV_COLOR_ENV + 2 * s + 1, setField(r, 2, 2, tex));
}
void GXSetTevSwapModeTable(GXTevSwapSel t, GXTevColorChan r, GXTevColorChan gch, GXTevColorChan b, GXTevColorChan a) {
    uint32_t k0 = setField(g.bp[BP_TEV_KSEL + 2 * t], 2, 0, r);
    apiBP(BP_TEV_KSEL + 2 * t, setField(k0, 2, 2, gch));
    uint32_t k1 = setField(g.bp[BP_TEV_KSEL + 2 * t + 1], 2, 0, b);
    apiBP(BP_TEV_KSEL + 2 * t + 1, setField(k1, 2, 2, a));
}
void GXSetTevClampMode(void) {}
void GXSetAlphaCompare(GXCompare c0, u8 ref0, GXAlphaOp op, GXCompare c1, u8 ref1) {
    uint32_t r = setField(0, 8, 0, ref0);
    r = setField(r, 8, 8, ref1);
    r = setField(r, 3, 16, c0);
    r = setField(r, 3, 19, c1);
    apiBP(BP_ALPHACOMPARE, setField(r, 2, 22, op));
}
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    uint32_t zf = fmt == GX_TF_Z8 ? 0 : fmt == GX_TF_Z16 ? 1 : 2;
    apiBP(BP_ZTEX_BIAS, uint32_t(bias));
    apiBP(BP_ZTEX2, setField(zf, 2, 2, op));
}
void GXSetTevOrder(GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    static const uint8_t c2r[9] = {0, 1, 0, 1, 0, 1, 7, 5, 6};
    uint32_t reg = BP_TREF + (s >> 1);
    int sh = (s & 1) ? 12 : 0;
    uint32_t tmap = uint32_t(map) & 0xFF;
    bool enable = tmap != 0xFF && !(uint32_t(map) & 0x100);
    uint32_t r = g.bp[reg];
    r = setField(r, 3, sh, tmap == 0xFF ? 0 : tmap & 7);
    r = setField(r, 3, sh + 3, coord == GX_TEXCOORD_NULL ? 0 : uint32_t(coord) & 7);
    r = setField(r, 1, sh + 6, enable);
    r = setField(r, 3, sh + 7, uint32_t(color) < 9 ? c2r[color] : 7);
    apiBP(reg, r);
}
void GXSetNumTevStages(u8 n) { bpField(BP_GENMODE, 4, 10, n ? n - 1 : 0); }

// ================================================================== GXBump
void GXSetTevIndirect(GXTevStageID s, GXIndTexStageID ind, GXIndTexFormat fmt, GXIndTexBiasSel bias, GXIndTexMtxID mtx,
                      GXIndTexWrap ws, GXIndTexWrap wt, GXBool addPrev, GXBool utcLod, GXIndTexAlphaSel alpha) {
    uint32_t r = setField(0, 2, 0, ind);
    r = setField(r, 2, 2, fmt);
    r = setField(r, 3, 4, bias);
    r = setField(r, 2, 7, alpha);
    r = setField(r, 4, 9, mtx);
    r = setField(r, 3, 13, ws);
    r = setField(r, 3, 16, wt);
    r = setField(r, 1, 19, utcLod);
    r = setField(r, 1, 20, addPrev);
    apiBP(BP_IND_CMD + s, r);
}
void GXSetIndTexMtx(GXIndTexMtxID id, f32 offset[2][3], s8 scaleExp) {
    int m;
    if (id >= GX_ITM_0 && id <= GX_ITM_2) m = id - GX_ITM_0;
    else if (id >= GX_ITM_S0 && id <= GX_ITM_S2) m = id - GX_ITM_S0;
    else if (id >= GX_ITM_T0 && id <= GX_ITM_T2) m = id - GX_ITM_T0;
    else return;
    uint32_t s = uint32_t(scaleExp + 17);
    auto fx = [](float f) { return uint32_t(int32_t(f * 1024.0f)) & 0x7FF; };
    uint32_t r0 = fx(offset[0][0]) | fx(offset[1][0]) << 11 | (s & 3) << 22;
    uint32_t r1 = fx(offset[0][1]) | fx(offset[1][1]) << 11 | ((s >> 2) & 3) << 22;
    uint32_t r2 = fx(offset[0][2]) | fx(offset[1][2]) << 11 | ((s >> 4) & 3) << 22;
    apiBP(BP_IND_MTX + 3 * m, r0);
    apiBP(BP_IND_MTX + 3 * m + 1, r1);
    apiBP(BP_IND_MTX + 3 * m + 2, r2);
}
void GXSetIndTexCoordScale(GXIndTexStageID ind, GXIndTexScale ss, GXIndTexScale ts) {
    uint32_t reg = ind < 2 ? BP_RAS1_SS0 : BP_RAS1_SS1;
    int sh = (ind & 1) ? 8 : 0;
    uint32_t r = setField(g.bp[reg], 4, sh, ss);
    apiBP(reg, setField(r, 4, sh + 4, ts));
}
void GXSetIndTexOrder(GXIndTexStageID ind, GXTexCoordID coord, GXTexMapID map) {
    uint32_t r = setField(g.bp[BP_RAS1_IREF], 3, 6 * ind, uint32_t(map) & 7);
    apiBP(BP_RAS1_IREF, setField(r, 3, 6 * ind + 3, uint32_t(coord) & 7));
}
void GXSetNumIndStages(u8 n) { bpField(BP_GENMODE, 3, 16, n); }
void GXSetTevDirect(GXTevStageID s) { apiBP(BP_IND_CMD + s, 0); }
void GXSetTevIndWarp(GXTevStageID s, GXIndTexStageID ind, u8 signedOffset, u8 replace, GXIndTexMtxID mtx) {
    GXIndTexWrap w = replace ? GX_ITW_0 : GX_ITW_OFF;
    GXSetTevIndirect(s, ind, GX_ITF_8, signedOffset ? GX_ITB_STU : GX_ITB_NONE, mtx, w, w, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
}
void GXSetTevIndTile(GXTevStageID s, GXIndTexStageID ind, u16 tileS, u16 tileT, u16 spS, u16 spT, GXIndTexFormat fmt,
                     GXIndTexMtxID mtx, GXIndTexBiasSel bias, GXIndTexAlphaSel alpha) {
    auto wrapFor = [](u16 size) {
        switch (size) {
        case 256: return GX_ITW_256;
        case 128: return GX_ITW_128;
        case 64: return GX_ITW_64;
        case 32: return GX_ITW_32;
        case 16: return GX_ITW_16;
        default: return GX_ITW_OFF;
        }
    };
    float m[2][3] = {{spS / 1024.0f, 0, 0}, {0, spT / 1024.0f, 0}};
    GXSetIndTexMtx(mtx, m, 10);
    GXSetTevIndirect(s, ind, fmt, bias, mtx, wrapFor(tileS), wrapFor(tileT), GX_FALSE, GX_TRUE, alpha);
}
void GXSetTevIndBumpST(GXTevStageID s, GXIndTexStageID ind, GXIndTexMtxID mtx) {
    GXIndTexMtxID sm = GX_ITM_S0, tm = GX_ITM_T0;
    if (mtx == GX_ITM_1) { sm = GX_ITM_S1; tm = GX_ITM_T1; }
    else if (mtx == GX_ITM_2) { sm = GX_ITM_S2; tm = GX_ITM_T2; }
    GXSetTevIndirect(s, ind, GX_ITF_8, GX_ITB_ST, sm, GX_ITW_0, GX_ITW_0, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
    GXSetTevIndirect(GXTevStageID(s + 1), ind, GX_ITF_8, GX_ITB_ST, tm, GX_ITW_0, GX_ITW_0, GX_TRUE, GX_FALSE, GX_ITBA_OFF);
    GXSetTevIndirect(GXTevStageID(s + 2), ind, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, GX_TRUE, GX_FALSE, GX_ITBA_OFF);
}
void GXSetTevIndBumpXYZ(GXTevStageID s, GXIndTexStageID ind, GXIndTexMtxID mtx) {
    GXSetTevIndirect(s, ind, GX_ITF_8, GX_ITB_STU, mtx, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
}
void GXSetTevIndRepeat(GXTevStageID s) {
    GXSetTevIndirect(s, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_0, GX_ITW_0, GX_TRUE, GX_FALSE, GX_ITBA_OFF);
}

// ================================================================== GXPixel
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    float A, B, C;
    if (farz == nearz || endz == startz) {
        A = 0.0f; B = 0.5f; C = 0.0f;
    } else {
        A = (farz * nearz) / ((farz - nearz) * (endz - startz));
        B = farz / (farz - nearz);
        C = startz / (endz - startz);
    }
    float bm = B;
    int be = 0;
    while (bm > 1.0f) { bm *= 0.5f; be++; }
    while (bm > 0.0f && bm < 0.5f) { bm *= 2.0f; be--; }
    float a = A / float(1 << (be + 1 > 0 ? be + 1 : 0));
    if (be + 1 < 0) a = A * float(1 << -(be + 1));
    uint32_t bmant = port_cvt_fp2unsigned(8.388638e6f * bm);
    uint32_t bshift = uint32_t(be + 1);
    uint32_t ah = fbits(a), ch = fbits(C);
    uint32_t f0 = ((ah >> 12) & 0x7FF) | ((ah >> 23) & 0xFF) << 11 | (ah >> 31) << 19;
    uint32_t f3 = ((ch >> 12) & 0x7FF) | ((ch >> 23) & 0xFF) << 11 | (ch >> 31) << 19 | (uint32_t(type) & 7) << 21;
    apiBP(BP_FOG0, f0);
    apiBP(BP_FOG1, bmant & 0xFFFFFF);
    apiBP(BP_FOG2, bshift & 31);
    apiBP(BP_FOG3, f3);
    apiBP(BP_FOG_COLOR, uint32_t(color.r) << 16 | uint32_t(color.g) << 8 | color.b);
}
// Not in the DOL (UNUSED in marioUS.MAP) and never called by the game.
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, f32 projmtx[4][4]) {
    float xi, iw = 2.0f / width;
    for (int i = 0; i < 10; i++) {
        xi = (width * (i + 1) / 20.0f) * iw;  // normalised screen x of the sample
        float r = xi / projmtx[0][0];
        table->r[i] = u16(uint32_t(256.0f / sqrtf(1.0f + r * r)) & 0xFFF);
    }
}
void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) {
    if (enable && table) {
        for (int i = 0; i < 10; i += 2)
            apiBP(0xE9 + i / 2, uint32_t(table->r[i]) | uint32_t(table->r[i + 1]) << 12);
    }
    apiBP(BP_FOG_RANGE, (uint32_t(center) + 342) | (uint32_t(enable) << 10));
}
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    uint32_t r = g.bp[BP_CMODE0];
    r = setField(r, 1, 0, type == GX_BM_BLEND || type == GX_BM_SUBTRACT);
    r = setField(r, 1, 1, type == GX_BM_LOGIC);
    r = setField(r, 1, 11, type == GX_BM_SUBTRACT);
    r = setField(r, 4, 12, op);
    r = setField(r, 3, 8, src);
    r = setField(r, 3, 5, dst);
    apiBP(BP_CMODE0, r);
}
void GXSetColorUpdate(GXBool e) { bpField(BP_CMODE0, 1, 3, e); }
void GXSetAlphaUpdate(GXBool e) { bpField(BP_CMODE0, 1, 4, e); }
void GXSetDither(GXBool e) { bpField(BP_CMODE0, 1, 2, e); }
void GXSetZMode(GXBool enable, GXCompare func, GXBool update) {
    apiBP(BP_ZMODE, uint32_t(enable) | uint32_t(func) << 1 | uint32_t(update) << 4);
}
void GXSetZCompLoc(GXBool before) { bpField(BP_PE_CONTROL, 1, 6, before); }
void GXSetPixelFmt(GXPixelFmt pix, GXZFmt16 z) {
    uint32_t r = setField(g.bp[BP_PE_CONTROL], 3, 0, pix);
    apiBP(BP_PE_CONTROL, setField(r, 3, 3, z));
    bpField(BP_GENMODE, 1, 9, pix == GX_PF_RGB565_Z16);
}
void GXSetDstAlpha(GXBool enable, u8 alpha) { apiBP(BP_CMODE1, uint32_t(alpha) | uint32_t(enable) << 8); }
void GXSetFieldMask(GXBool odd, GXBool even) { apiBP(BP_FIELDMASK, uint32_t(even) | uint32_t(odd) << 1); }
void GXSetFieldMode(GXBool field, GXBool half) {
    bpField(BP_LPSIZE, 1, 22, half);
    apiBP(BP_FIELDMODE, field);
}

// peek/poke: the EFB is a GL framebuffer, only reads are supported
void GXPokeAlphaMode(GXCompare, u8) {}
void GXPokeAlphaRead(GXAlphaReadMode) {}
void GXPokeAlphaUpdate(GXBool) {}
void GXPokeBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}
void GXPokeColorUpdate(GXBool) {}
void GXPokeDstAlpha(GXBool, u8) {}
void GXPokeDither(GXBool) {}
void GXPokeZMode(GXBool, GXCompare, GXBool) {}
void GXPokeARGB(u16, u16, u32) {}
void GXPeekARGB(u16 x, u16 y, u32* color) { *color = peekColor(x, y); }
void GXPeekZ(u16 x, u16 y, u32* z) { *z = peekZ(x, y); }
void GXPokeZ(u16, u16, u32) {}
u32 GXCompressZ16(u32 z24, GXZFmt16) { return z24 >> 8; }
u32 GXDecompressZ16(u32 z16, GXZFmt16) { return z16 << 8; }

// ================================================================== GXFrameBuf
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    s_dispSrc = setField(setField(0, 10, 0, left), 10, 10, top);
    s_dispSize = setField(setField(0, 10, 0, wd - 1u), 10, 10, ht - 1u);
}
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    s_texSrc = setField(setField(0, 10, 0, left), 10, 10, top);
    s_texSize = setField(setField(0, 10, 0, wd - 1u), 10, 10, ht - 1u);
}
void GXSetDispCopyDst(u16 wd, u16) { s_dispStride = uint32_t(wd) * 2 / 32; }
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    uint32_t f = uint32_t(fmt) & 0xF;
    if (fmt == GX_TF_Z16) f = 0xB;
    bool intensity = fmt == GX_TF_I4 || fmt == GX_TF_I8 || fmt == GX_TF_IA4 || fmt == GX_TF_IA8 || fmt == GX_CTF_YUVA8;
    s_texCtrl = setField(s_texCtrl, 2, 15, intensity ? 3 : 2);
    s_texCopyZ = (uint32_t(fmt) & _GX_TF_ZTF) != 0;
    s_texCtrl = setField(s_texCtrl, 1, 3, (f >> 3) & 1);
    s_texCtrl = setField(s_texCtrl, 3, 4, f & 7);
    s_texCtrl = setField(s_texCtrl, 1, 9, mipmap);
    // bit 13 is unused by the hardware copy path here: remember the Z-ness of the
    // format for the renderer (the SDK does this by switching the pixel format)
    s_texStride = GXGetTexBufferSize(wd, ht, fmt, GX_FALSE, 0) / (ht ? ht : 1);
}
void GXSetDispCopyFrame2Field(GXCopyMode mode) {
    s_dispCtrl = setField(s_dispCtrl, 2, 12, mode);
    s_texCtrl = setField(s_texCtrl, 2, 12, 0);
}
void GXSetCopyClamp(GXFBClamp clamp) {
    s_dispCtrl = setField(s_dispCtrl, 2, 0, clamp);
    s_texCtrl = setField(s_texCtrl, 2, 0, clamp);
}
u32 GXSetDispCopyYScale(f32 vscale) {
    uint32_t s = port_cvt_fp2unsigned(256.0f / vscale) & 0x1FF;
    s_dispYScale = s;
    s_dispCtrl = setField(s_dispCtrl, 1, 10, s != 256);
    uint32_t ht = getField(s_dispSize, 10, 10) + 1;
    return u32(GXGetNumXfbLines(u16(ht), vscale));
}
void GXSetCopyClear(GXColor c, u32 z) {
    apiBP(BP_CLEAR_AR, uint32_t(c.a) << 8 | c.r);
    apiBP(BP_CLEAR_GB, uint32_t(c.g) << 8 | c.b);
    apiBP(BP_CLEAR_Z, uint32_t(z) & 0xFFFFFF);
}
void GXSetCopyFilter(GXBool aa, const u8 pattern[12][2], GXBool vf, const u8 vfilter[7]) {
    (void)aa; (void)pattern;
    uint32_t v0 = 0, v1 = 0;
    if (vf && vfilter) {
        v0 = uint32_t(vfilter[0]) | uint32_t(vfilter[1]) << 6 | uint32_t(vfilter[2]) << 12 | uint32_t(vfilter[3]) << 18;
        v1 = uint32_t(vfilter[4]) | uint32_t(vfilter[5]) << 6 | uint32_t(vfilter[6]) << 12;
    } else {
        v0 = 0 | 0 << 6 | 21 << 12 | 22 << 18;
        v1 = 21 | 0 << 6 | 0 << 12;
    }
    apiBP(0x53, v0);
    apiBP(0x54, v1);
}
void GXSetDispCopyGamma(GXGamma gamma) { s_dispCtrl = setField(s_dispCtrl, 2, 7, gamma); }

static void doCopy(void* dest, GXBool clear, bool disp) {
    apiBP(BP_COPY_SRC_TL, disp ? s_dispSrc : s_texSrc);
    apiBP(BP_COPY_SRC_WH, disp ? s_dispSize : s_texSize);
    apiBP(BP_COPY_STRIDE, disp ? s_dispStride : s_texStride);
    if (disp) apiBP(BP_COPY_YSCALE, s_dispYScale);
    // the renderer needs the host pointer; commands carry a physical address
    apiBP(BP_COPY_DST, ptrToPhys(dest) >> 5);
    if (!s_recording) g.copyDest = dest;  // keep the full host pointer (64-bit hosts)
    uint32_t ctrl = disp ? s_dispCtrl : s_texCtrl;
    ctrl = setField(ctrl, 1, 11, clear);
    ctrl = setField(ctrl, 1, 14, disp);
    if (!disp && s_texCopyZ) {
        // the SDK switches PE_CONTROL to Z24 for depth copies
        uint32_t pe = g.bp[BP_PE_CONTROL];
        apiBP(BP_PE_CONTROL, setField(pe, 3, 0, 3));
        apiBP(BP_COPY_EXEC, ctrl);
        apiBP(BP_PE_CONTROL, pe);
    } else {
        apiBP(BP_COPY_EXEC, ctrl);
    }
}
void GXCopyDisp(void* dest, GXBool clear) { doCopy(dest, clear, true); }
void GXCopyTex(void* dest, GXBool clear) { doCopy(dest, clear, false); }
void GXClearBoundingBox(void) {}
void GXReadBoundingBox(u16* l, u16* t, u16* r, u16* b) { *l = 0; *t = 0; *r = 639; *b = 527; }

u16 GXGetNumXfbLines(u16 efbHeight, float yScale) {
    uint32_t iScale = port_cvt_fp2unsigned(256.0f / yScale) & 0x1FF;
    if (!iScale) return efbHeight;
    uint32_t n = ((uint32_t(efbHeight) - 1) * 256) / iScale + 1;
    if (iScale > 0x80 && iScale < 0x100) {
        while (iScale % 2 == 0) iScale /= 2;
        if (efbHeight % iScale == 0) n++;
    }
    if (n > 1024) n = 1024;
    return u16(n);
}
float GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    float f = float(xfbHeight) / float(efbHeight);
    uint32_t iScale = port_cvt_fp2unsigned(256.0f / f) & 0x1FF;
    // step the integer scale until the line count matches the request
    for (int guard = 0; guard < 512 && iScale > 1; guard++) {
        float s = 256.0f / float(iScale);
        uint16_t n = GXGetNumXfbLines(efbHeight, s);
        if (n == xfbHeight) return s;
        if (n > xfbHeight) iScale++;
        else iScale--;
    }
    return f;
}
void GXAdjustForOverscan(GXRenderModeObj* in, GXRenderModeObj* out, u16 hor, u16 ver) {
    if (in != out) *out = *in;
    out->fbWidth = u16(in->fbWidth - 2 * hor);
    out->efbHeight = u16(in->efbHeight - (in->efbHeight * 2 * ver) / in->xfbHeight);
    if (in->xFBmode == VI_XFBMODE_SF && !in->field_rendering) out->xfbHeight = u16(in->xfbHeight - ver);
    else out->xfbHeight = u16(in->xfbHeight - 2 * ver);
    out->viWidth = u16(in->viWidth - 2 * hor);
    out->viHeight = u16(in->viHeight - 2 * ver);
    out->viXOrigin = u16(in->viXOrigin + hor);
    out->viYOrigin = u16(in->viYOrigin + ver);
}

// ================================================================== GXTexture
static const uint8_t kTileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileBytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};

static uint32_t levelSize(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t f = fmt & 0xF;
    // copy formats: map to their storage class
    if (fmt & (_GX_TF_CTF | _GX_TF_ZTF)) {
        switch (fmt) {
        case GX_CTF_R4: case GX_CTF_Z4: f = GX_TF_I4; break;
        case GX_CTF_RA4: case GX_CTF_A8: case GX_CTF_R8: case GX_CTF_G8: case GX_CTF_B8: case GX_TF_Z8:
        case GX_CTF_Z8M: case GX_CTF_Z8L: f = GX_TF_I8; break;
        case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_TF_Z16: case GX_CTF_Z16L: f = GX_TF_IA8; break;
        default: f = GX_TF_RGBA8; break;
        }
    }
    uint32_t tw = kTileW[f], th = kTileH[f];
    if (!tw) return 0;
    uint32_t cols = (w + tw - 1) / tw, rows = (h + th - 1) / th;
    return cols * rows * kTileBytes[f];
}
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 maxLod) {
    uint32_t w = width, h = height, total = 0;
    uint32_t levels = mipmap ? maxLod : 1;
    if (levels == 0) levels = 1;
    for (uint32_t i = 0; i < levels; i++) {
        total += levelSize(uint32_t(format), w, h);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

static void texObjInit(TexObjInt* t, void* image, u16 w, u16 h, uint32_t fmt, GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap) {
    memset(t, 0, sizeof(*t));
    t->image = image;
    t->fmt = uint8_t(fmt);
    t->mipmap = mipmap;
    uint32_t m0 = setField(0, 2, 0, ws);
    m0 = setField(m0, 2, 2, wt);
    m0 = setField(m0, 1, 4, 1);
    m0 = setField(m0, 3, 5, mipmap ? 6 : 4);
    m0 = setField(m0, 1, 8, 1);
    t->mode0 = m0;
    uint32_t maxLod = 0;
    if (mipmap) {
        uint32_t m = w > h ? w : h;
        while (m > 1) { m >>= 1; maxLod++; }
    }
    t->mode1 = setField(0, 8, 8, maxLod * 16);
    t->image0 = setField(setField(setField(0, 10, 0, w - 1u), 10, 10, h - 1u), 4, 20, fmt & 0xF);
}
void GXInitTexObj(GXTexObj* obj, void* image, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap) {
    texObjInit(reinterpret_cast<TexObjInt*>(obj), image, w, h, fmt, ws, wt, mipmap);
}
void GXInitTexObjCI(GXTexObj* obj, void* image, u16 w, u16 h, GXCITexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap, u32 tlut) {
    TexObjInt* t = reinterpret_cast<TexObjInt*>(obj);
    texObjInit(t, image, w, h, fmt, ws, wt, mipmap);
    t->isCI = 1;
    t->tlutName = uint32_t(tlut);
}
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter minF, GXTexFilter magF, f32 minLod, f32 maxLod, f32 bias, GXBool biasClamp,
                     GXBool edgeLod, GXAnisotropy aniso) {
    TexObjInt* t = reinterpret_cast<TexObjInt*>(obj);
    if (bias < -4.0f) bias = -4.0f;
    if (bias > 3.99f) bias = 3.99f;
    uint32_t m0 = t->mode0;
    m0 = setField(m0, 8, 9, uint32_t(int32_t(bias * 32.0f)) & 0xFF);
    m0 = setField(m0, 1, 4, magF == GX_LINEAR);
    m0 = setField(m0, 3, 5, kFiltHw[minF < 6 ? minF : 1]);
    m0 = setField(m0, 1, 8, !edgeLod);
    m0 = setField(m0, 1, 21, biasClamp);
    m0 = setField(m0, 2, 19, aniso);
    t->mode0 = m0;
    if (minLod < 0) minLod = 0;
    if (minLod > 10) minLod = 10;
    if (maxLod < 0) maxLod = 0;
    if (maxLod > 10) maxLod = 10;
    t->mode1 = setField(setField(0, 8, 0, uint32_t(minLod * 16.0f)), 8, 8, uint32_t(maxLod * 16.0f));
}
void GXInitTexObjData(GXTexObj* obj, void* image) { reinterpret_cast<TexObjInt*>(obj)->image = image; }
void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t) {
    TexObjInt* o = reinterpret_cast<TexObjInt*>(obj);
    o->mode0 = setField(setField(o->mode0, 2, 0, s), 2, 2, t);
}
void GXInitTexObjTlut(GXTexObj* obj, u32 tlut) { reinterpret_cast<TexObjInt*>(obj)->tlutName = uint32_t(tlut); }
void GXInitTexObjUserData(GXTexObj* obj, void* user) { reinterpret_cast<TexObjInt*>(obj)->user = user; }
void* GXGetTexObjUserData(const GXTexObj* obj) { return reinterpret_cast<const TexObjInt*>(obj)->user; }

void GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion*, GXTexMapID id) {
    const TexObjInt* t = reinterpret_cast<const TexObjInt*>(obj);
    int map = int(id) & 7;
    apiBP(bpTexReg(BP_TX_MODE0, map), t->mode0);
    apiBP(bpTexReg(BP_TX_MODE1, map), t->mode1);
    apiBP(bpTexReg(BP_TX_IMAGE0, map), t->image0);
    apiBP(bpTexReg(BP_TX_IMAGE3, map), ptrToPhys(t->image) >> 5);
    if (t->isCI) {
        const TlutRegionInt* r = reinterpret_cast<const TlutRegionInt*>(s_tlutRegionCb(t->tlutName));
        uint32_t idx = uint32_t(reinterpret_cast<const GXTlutRegion*>(r) - s_tlutRegions);
        uint32_t fmt = idx < 20 ? s_tlutLoaded[idx].fmt : 0;
        apiBP(bpTexReg(BP_TX_TLUT, map), ((r->tmemAddr - TMEM_TLUT_BASE) >> 9) | fmt << 10);
    }
    if (!s_recording) g.texImage[map] = static_cast<const uint8_t*>(static_cast<const void*>(t->image));
}
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if (s_texRegionCb) s_texRegionCb(obj, id);
    GXLoadTexObjPreLoaded(obj, nullptr, id);
}

void GXInitTlutObj(GXTlutObj* obj, void* lut, GXTlutFmt fmt, u16 n) {
    TlutObjInt* t = reinterpret_cast<TlutObjInt*>(obj);
    t->fmt = uint32_t(fmt);
    t->data = lut;
    t->entries = n;
}
void GXLoadTlut(GXTlutObj* obj, u32 name) {
    const TlutObjInt* t = reinterpret_cast<const TlutObjInt*>(obj);
    GXTlutRegion* reg = s_tlutRegionCb(name);
    const TlutRegionInt* r = reinterpret_cast<const TlutRegionInt*>(reg);
    uint32_t idx = uint32_t(reg - s_tlutRegions);
    if (idx < 20) s_tlutLoaded[idx] = *t;
    if (s_recording) {
        apiBP(BP_LOADTLUT0, ptrToPhys(t->data) >> 5);
        apiBP(BP_LOADTLUT1, ((r->tmemAddr - TMEM_TLUT_BASE) >> 9) | uint32_t((t->entries + 15) / 16) << 10);
        return;
    }
    uint32_t off = r->tmemAddr - TMEM_TLUT_BASE;
    uint32_t bytes = uint32_t(t->entries) * 2;
    if (off + bytes > TMEM_TLUT_SIZE) bytes = TMEM_TLUT_SIZE - off;
    onStateChange();
    if (t->data) memcpy(g.tlutMem + off, t->data, bytes);
}
void GXInitTexCacheRegion(GXTexRegion* region, u8 is32b, u32 even, GXTexCacheSize, u32 odd, GXTexCacheSize) {
    TexRegionInt* r = reinterpret_cast<TexRegionInt*>(region);
    r->tmemEven = uint32_t(even);
    r->tmemOdd = uint32_t(odd);
    r->is32b = is32b;
    r->cached = 1;
}
void GXInitTexPreLoadRegion(GXTexRegion* region, u32 even, u32, u32 odd, u32) {
    TexRegionInt* r = reinterpret_cast<TexRegionInt*>(region);
    r->tmemEven = uint32_t(even);
    r->tmemOdd = uint32_t(odd);
    r->cached = 0;
}
void GXInitTlutRegion(GXTlutRegion* region, u32 addr, GXTlutSize size) {
    TlutRegionInt* r = reinterpret_cast<TlutRegionInt*>(region);
    r->tmemAddr = uint32_t(addr);
    r->size = uint32_t(size);
}
void GXInvalidateTexRegion(GXTexRegion*) { textureInvalidateAll(); }
void GXInvalidateTexAll(void) {
    flushBatch();
    textureInvalidateAll();
}
GXTexRegionCallback GXSetTexRegionCallback(GXTexRegionCallback f) {
    GXTexRegionCallback old = s_texRegionCb;
    s_texRegionCb = f;
    return old;
}
GXTlutRegionCallback GXSetTlutRegionCallback(GXTlutRegionCallback f) {
    GXTlutRegionCallback old = s_tlutRegionCb;
    s_tlutRegionCb = f;
    return old;
}
void GXPreLoadEntireTexture(GXTexObj*, GXTexRegion*) {}
void GXSetTexCoordScaleManually(GXTexCoordID coord, u8 enable, u16 ss, u16 ts) {
    if (coord >= 8) return;
    g.tcManual[coord] = enable != 0;
    if (enable) {
        bpField(BP_SU_SSIZE + 2 * coord, 16, 0, ss - 1u);
        bpField(BP_SU_SSIZE + 2 * coord + 1, 16, 0, ts - 1u);
    }
}
void GXSetTexCoordCylWrap(GXTexCoordID coord, u8 s, u8 t) {
    if (coord >= 8) return;
    bpField(BP_SU_SSIZE + 2 * coord, 1, 17, s);
    bpField(BP_SU_SSIZE + 2 * coord + 1, 1, 17, t);
}
void GXSetTexCoordBias(GXTexCoordID coord, u8 s, u8 t) {
    if (coord >= 8) return;
    bpField(BP_SU_SSIZE + 2 * coord, 1, 16, s);
    bpField(BP_SU_SSIZE + 2 * coord + 1, 1, 16, t);
}

// texture-object getters
GXBool GXGetTexObjMipMap(const GXTexObj* o) { return reinterpret_cast<const TexObjInt*>(o)->mipmap; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* o) { return GXTexFmt(reinterpret_cast<const TexObjInt*>(o)->fmt); }
u16 GXGetTexObjWidth(const GXTexObj* o) { return u16(getField(reinterpret_cast<const TexObjInt*>(o)->image0, 10, 0) + 1); }
u16 GXGetTexObjHeight(const GXTexObj* o) { return u16(getField(reinterpret_cast<const TexObjInt*>(o)->image0, 10, 10) + 1); }
GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* o) { return GXTexWrapMode(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 2, 0)); }
GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* o) { return GXTexWrapMode(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 2, 2)); }
void* GXGetTexObjData(const GXTexObj* o) { return const_cast<void*>(static_cast<const void*>(reinterpret_cast<const TexObjInt*>(o)->image)); }
void GXGetTexObjAll(const GXTexObj* o, void** image, u16* w, u16* h, GXTexFmt* fmt, GXTexWrapMode* ws, GXTexWrapMode* wt, u8* mip) {
    *image = GXGetTexObjData(o);
    *w = GXGetTexObjWidth(o);
    *h = GXGetTexObjHeight(o);
    *fmt = GXGetTexObjFmt(o);
    *ws = GXGetTexObjWrapS(o);
    *wt = GXGetTexObjWrapT(o);
    *mip = GXGetTexObjMipMap(o);
}
GXTexFilter GXGetTexObjMinFilt(const GXTexObj* o) { return GXTexFilter(kFiltGx[getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 3, 5)]); }
GXTexFilter GXGetTexObjMagFilt(const GXTexObj* o) { return GXTexFilter(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 1, 4)); }
f32 GXGetTexObjMinLOD(const GXTexObj* o) { return getField(reinterpret_cast<const TexObjInt*>(o)->mode1, 8, 0) / 16.0f; }
f32 GXGetTexObjMaxLOD(const GXTexObj* o) { return getField(reinterpret_cast<const TexObjInt*>(o)->mode1, 8, 8) / 16.0f; }
f32 GXGetTexObjLODBias(const GXTexObj* o) { return int8_t(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 8, 9)) / 32.0f; }
GXBool GXGetTexObjBiasClamp(const GXTexObj* o) { return GXBool(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 1, 21)); }
GXBool GXGetTexObjEdgeLOD(const GXTexObj* o) { return GXBool(!getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 1, 8)); }
GXAnisotropy GXGetTexObjMaxAniso(const GXTexObj* o) { return GXAnisotropy(getField(reinterpret_cast<const TexObjInt*>(o)->mode0, 2, 19)); }
u32 GXGetTexObjTlut(const GXTexObj* o) { return reinterpret_cast<const TexObjInt*>(o)->tlutName; }
void GXGetTexObjLODAll(const GXTexObj* o, GXTexFilter* minF, GXTexFilter* magF, f32* minLod, f32* maxLod, f32* bias, u8* biasClamp,
                       u8* edgeLod, GXAnisotropy* aniso) {
    *minF = GXGetTexObjMinFilt(o);
    *magF = GXGetTexObjMagFilt(o);
    *minLod = GXGetTexObjMinLOD(o);
    *maxLod = GXGetTexObjMaxLOD(o);
    *bias = GXGetTexObjLODBias(o);
    *biasClamp = GXGetTexObjBiasClamp(o);
    *edgeLod = GXGetTexObjEdgeLOD(o);
    *aniso = GXGetTexObjMaxAniso(o);
}
void GXGetTlutObjAll(const GXTlutObj* o, void** data, GXTlutFmt* fmt, u16* n) {
    const TlutObjInt* t = reinterpret_cast<const TlutObjInt*>(o);
    *data = const_cast<void*>(static_cast<const void*>(t->data));
    *fmt = GXTlutFmt(t->fmt);
    *n = t->entries;
}
void* GXGetTlutObjData(const GXTlutObj* o) { return const_cast<void*>(static_cast<const void*>(reinterpret_cast<const TlutObjInt*>(o)->data)); }
GXTlutFmt GXGetTlutObjFmt(const GXTlutObj* o) { return GXTlutFmt(reinterpret_cast<const TlutObjInt*>(o)->fmt); }
u16 GXGetTlutObjNumEntries(const GXTlutObj* o) { return reinterpret_cast<const TlutObjInt*>(o)->entries; }
void GXGetTexRegionAll(const GXTexRegion* region, u8* cached, u8* is32b, u32* even, u32* sizeEven, u32* odd, u32* sizeOdd) {
    const TexRegionInt* r = reinterpret_cast<const TexRegionInt*>(region);
    *cached = r->cached;
    *is32b = r->is32b;
    *even = r->tmemEven;
    *odd = r->tmemOdd;
    *sizeEven = 0x8000;
    *sizeOdd = 0x8000;
}
void GXGetTlutRegionAll(const GXTlutRegion* region, u32* addr, GXTlutSize* size) {
    const TlutRegionInt* r = reinterpret_cast<const TlutRegionInt*>(region);
    *addr = r->tmemAddr;
    *size = GXTlutSize(r->size);
}

// ================================================================== GXPerf (no counters on the host)
void GXSetGPMetric(GXPerf0, GXPerf1) {}
void GXReadGPMetric(u32* c0, u32* c1) { *c0 = 0; *c1 = 0; }
void GXClearGPMetric(void) {}
u32 GXReadGP0Metric(void) { return 0; }
u32 GXReadGP1Metric(void) { return 0; }
void GXReadMemMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f, u32* gg, u32* h, u32* i, u32* j) {
    *a = *b = *c = *d = *e = *f = *gg = *h = *i = *j = 0;
}
void GXClearMemMetric(void) {}
// top/bottom pixels in and colour pixels in all report the pixels that
// passed (plus 4 per triangle, see gx_render.cpp); the rest read zero.
void GXReadPixMetric(u32* topIn, u32* topOut, u32* botIn, u32* botOut, u32* clrIn, u32* copyClks) {
    u32 n = pixMetricRead();
    *topIn = *botIn = *clrIn = n;
    *topOut = *botOut = *copyClks = 0;
}
void GXClearPixMetric(void) { pixMetricClear(); }
void GXSetVCacheMetric(GXVCachePerf) {}
void GXReadVCacheMetric(u32* a, u32* b, u32* c) { *a = *b = *c = 0; }
void GXClearVCacheMetric(void) {}
void GXInitXfRasMetric(void) {}
void GXReadXfRasMetric(u32* a, u32* b, u32* c, u32* d) { *a = *b = *c = *d = 0; }
u32 GXReadClksPerVtx(void) { return 0; }


// ================================================================== GXDraw (SDK helper shapes)
static void drawWithPosNrm(void (*body)()) {
    uint32_t lo = g.cpVcdLo, hi = g.cpVcdHi;
    uint32_t A = g.cpVatA[GX_VTXFMT3], B = g.cpVatB[GX_VTXFMT3], C = g.cpVatC[GX_VTXFMT3];
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    body();
    apiCP(CP_VCD_LO, lo);
    apiCP(CP_VCD_HI, hi);
    apiCP(CP_VAT_A + GX_VTXFMT3, A);
    apiCP(CP_VAT_B + GX_VTXFMT3, B);
    apiCP(CP_VAT_C + GX_VTXFMT3, C);
}
static void vtxPN(float x, float y, float z, float nx, float ny, float nz) {
    GXPC_WriteF32(x); GXPC_WriteF32(y); GXPC_WriteF32(z);
    GXPC_WriteF32(nx); GXPC_WriteF32(ny); GXPC_WriteF32(nz);
}
// The SDK's cube (GXDrawCube in the decomp's GXDraw.c): six quads with the
// corners on the unit sphere (+-0.57735026), wound clockwise seen from
// outside, i.e. front-facing. Games rely on both: Mario's occlusion probe
// (TMario::boxDrawPrepare) culls front faces and depth-tests the far side
// of a box that must stay above the floor he stands on.
static void cubeFace(float nx, float ny, float nz, float tx, float ty, float tz, float bx, float by, float bz) {
    const float k = 0.57735026f;
    vtxPN(k * (nx + tx + bx), k * (ny + ty + by), k * (nz + tz + bz), nx, ny, nz);
    vtxPN(k * (nx - tx + bx), k * (ny - ty + by), k * (nz - tz + bz), nx, ny, nz);
    vtxPN(k * (nx - tx - bx), k * (ny - ty - by), k * (nz - tz - bz), nx, ny, nz);
    vtxPN(k * (nx + tx - bx), k * (ny + ty - by), k * (nz + tz - bz), nx, ny, nz);
}
static void cubeBody() {
    GXBegin(GX_QUADS, GX_VTXFMT3, 24);
    cubeFace(-1, 0, 0, 0, 0, -1, 0, 1, 0);
    cubeFace(1, 0, 0, 0, 1, 0, 0, 0, -1);
    cubeFace(0, -1, 0, -1, 0, 0, 0, 0, 1);
    cubeFace(0, 1, 0, 0, 0, 1, -1, 0, 0);
    cubeFace(0, 0, -1, 0, -1, 0, 1, 0, 0);
    cubeFace(0, 0, 1, 1, 0, 0, 0, -1, 0);
}
static uint8_t s_sphMajor, s_sphMinor;
// The SDK's sphere (GXDrawSphere in GXDraw.c, gxsdkSphere): rings from the +z
// pole down, each strip emitting the next ring's vertex before the current
// one's, which sets the winding the sky's front-face culling relies on.
static void sphereBegin(uint16_t count) { GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, count); }
static void sphereVertex(float x, float y, float z) { vtxPN(x, y, z, x, y, z); }
static void sphereBody() { gxsdkSphere(s_sphMajor, s_sphMinor, sphereBegin, sphereVertex); }
// GXDrawCylinder is not in the DOL (UNUSED in marioUS.MAP) and never called.
static uint8_t s_cylEdges;
static void cylinderBody() {
    const float pi = 3.14159265f;
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, u16((s_cylEdges + 1) * 2));
    for (int j = 0; j <= s_cylEdges; j++) {
        float b = 2 * pi * j / s_cylEdges, x = cosf(b), y = sinf(b);
        vtxPN(x, y, 1, x, y, 0);
        vtxPN(x, y, -1, x, y, 0);
    }
}
void GXDrawCube(void) { drawWithPosNrm(cubeBody); }
void GXDrawSphere(u8 major, u8 minor) {
    s_sphMajor = major ? major : 1;
    s_sphMinor = minor ? minor : 3;
    drawWithPosNrm(sphereBody);
}
void GXDrawSphere1(u8 depth) { GXDrawSphere(u8(4 << depth), u8(8 << depth)); }
void GXDrawCylinder(u8 edges) {
    s_cylEdges = edges ? edges : 3;
    drawWithPosNrm(cylinderBody);
}
void GXDrawTorus(f32, u8, u8) {}
void GXDrawDodeca(void) {}
void GXDrawOctahedron(void) {}
void GXDrawIcosahedron(void) {}
u32 GXGenNormalTable(u8, f32*) { return 0; }

}  // extern "C"

// GXVerify.h declares these without extern "C", so they keep C++ linkage.
void GXSetVerifyLevel(GXWarningLevel) {}
GXVerifyCallback GXSetVerifyCallback(GXVerifyCallback cb) { return cb; }
