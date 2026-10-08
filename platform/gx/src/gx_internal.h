// Internal state of the host GX implementation.
//
// Model: the GameCube GPU is driven entirely by register writes (BP = pixel
// pipeline, CP = vertex loader, XF = transform unit) and draw commands.  Every
// GX/GD entry point, every byte written through the write-gather pipe and every
// display list ends up as one of those register writes, so the renderer reads
// its state from one register file and nothing else.  Register layouts are
// hardware facts, cross-checked against the decompiled SDK in decomp/libs/dolphin/src.
#ifndef SMS_GX_INTERNAL_H
#define SMS_GX_INTERNAL_H

#include <stdint.h>
#include <string>
#include <stddef.h>
#include <vector>

namespace gx {

// ---------------------------------------------------------------- registers
enum : uint32_t {
    XF_MEM_WORDS   = 0x680,   // 0x000-0x67F: matrices, normal matrices, post matrices, lights
    XF_REG_BASE    = 0x1000,
    XF_REG_COUNT   = 0x58,
    TMEM_TLUT_BASE = 0x80000, // TLUTs live in the upper half of TMEM
    TMEM_TLUT_SIZE = 0x80000,
};

// XF register offsets relative to 0x1000
enum {
    XFR_NUMCHANS = 0x09, XFR_AMB0 = 0x0A, XFR_MAT0 = 0x0C, XFR_COLOR0CTRL = 0x0E,
    XFR_ALPHA0CTRL = 0x10, XFR_DUALTEX = 0x12, XFR_MATIDX_A = 0x18, XFR_MATIDX_B = 0x19,
    XFR_VIEWPORT = 0x1A, XFR_PROJ = 0x20, XFR_NUMTEXGENS = 0x3F, XFR_TEXGEN = 0x40,
    XFR_POSTTEX = 0x50,
};

// BP register ids
enum {
    BP_GENMODE = 0x00, BP_IND_MTX = 0x06, BP_IND_IMASK = 0x0F, BP_IND_CMD = 0x10,
    BP_SCISSOR_TL = 0x20, BP_SCISSOR_BR = 0x21, BP_LPSIZE = 0x22, BP_RAS1_SS0 = 0x25,
    BP_RAS1_SS1 = 0x26, BP_RAS1_IREF = 0x27, BP_TREF = 0x28, BP_SU_SSIZE = 0x30,
    BP_ZMODE = 0x40, BP_CMODE0 = 0x41, BP_CMODE1 = 0x42, BP_PE_CONTROL = 0x43,
    BP_FIELDMASK = 0x44, BP_DRAWDONE = 0x45, BP_PE_TOKEN = 0x47, BP_PE_TOKEN_INT = 0x48,
    BP_COPY_SRC_TL = 0x49, BP_COPY_SRC_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D,
    BP_COPY_YSCALE = 0x4E, BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51,
    BP_COPY_EXEC = 0x52, BP_SCISSOR_OFS = 0x59, BP_LOADTLUT0 = 0x64, BP_LOADTLUT1 = 0x65,
    BP_TEXINVALIDATE = 0x66, BP_FIELDMODE = 0x68,
    BP_TX_MODE0 = 0x80, BP_TX_MODE1 = 0x84, BP_TX_IMAGE0 = 0x88, BP_TX_IMAGE1 = 0x8C,
    BP_TX_IMAGE2 = 0x90, BP_TX_IMAGE3 = 0x94, BP_TX_TLUT = 0x98,
    BP_TEV_COLOR_ENV = 0xC0, BP_TEV_REG = 0xE0, BP_FOG_RANGE = 0xE8, BP_FOG0 = 0xEE,
    BP_FOG1 = 0xEF, BP_FOG2 = 0xF0, BP_FOG3 = 0xF1, BP_FOG_COLOR = 0xF2,
    BP_ALPHACOMPARE = 0xF3, BP_ZTEX_BIAS = 0xF4, BP_ZTEX2 = 0xF5, BP_TEV_KSEL = 0xF6,
    BP_MASK = 0xFE,
};

// Texture-map register addressing: maps 0-3 at 0x80.., 4-7 at 0xA0..
inline int bpTexReg(int base, int map) { return base + (map & 3) + ((map & 4) ? 0x20 : 0); }

// CP register ids
enum {
    CP_MATIDX_A = 0x30, CP_MATIDX_B = 0x40, CP_VCD_LO = 0x50, CP_VCD_HI = 0x60,
    CP_VAT_A = 0x70, CP_VAT_B = 0x80, CP_VAT_C = 0x90, CP_ARRAY_BASE = 0xA0,
    CP_ARRAY_STRIDE = 0xB0,
};

struct EfbCopy;  // defined in gx_render.cpp

struct State {
    uint32_t bp[256];
    uint32_t kreg[8];         // konst colours share BP 0xE0-0xE7 with the TEV registers (bit 23 set)
    uint32_t bpMask;          // one-shot mask set by BP 0xFE
    uint32_t cpMatA, cpMatB, cpVcdLo, cpVcdHi;
    uint32_t cpVatA[8], cpVatB[8], cpVatC[8];
    const uint8_t* arrayBase[16];
    uint32_t arrayStride[16];
    bool arrayBigEndian[16];
    uint32_t xfReg[XF_REG_COUNT];
    uint32_t xfMem[XF_MEM_WORDS];
    const uint8_t* texImage[8];   // host pointer for each texture map
    void* copyDest;               // host pointer of the EFB copy destination (BP 0x4B)
    uint8_t tlutMem[TMEM_TLUT_SIZE];
    // manual texcoord scale (GXSetTexCoordScaleManually), per texcoord
    bool tcManual[8];
    // shadow values the GX getters report
    float projection[7];
    float viewport[6];
    uint32_t scissor[4];
    uint32_t cullMode;  // GXCullMode
    uint8_t lineWidth, pointSize;
    // draw sync
    uint16_t drawSyncToken;
};

extern State g;

// ---------------------------------------------------------------- memory
// Physical-address window used by 32-bit addresses inside command streams.
void* physToPtr(uint32_t phys);
uint32_t ptrToPhys(const void* p);
bool isBigEndianData(const void* p);
extern bool g_defaultArrayBE;
extern bool g_gxStats;  // SMS_GX_STATS: per-part timers run
extern uint32_t g_arrayGen;  // bumped by every write to g.arrayBase/Stride/BigEndian

// ---------------------------------------------------------------- register writes (gx_fifo.cpp)
void writeBP(uint32_t value);
void writeCP(uint8_t reg, uint32_t value);
void writeXF(uint16_t addr, uint32_t count, const uint32_t* words);
inline void writeXF1(uint16_t addr, uint32_t v) { writeXF(addr, 1, &v); }
void writeXFf(uint16_t addr, uint32_t count, const float* f);
void resetState();

// Command-stream parsing.  Big-endian bytes.
void runCommands(const uint8_t* data, uint32_t size);
void pipeWrite(const uint8_t* bytes, uint32_t n);
bool pipeIdle();
void setPipeRedirect(uint8_t* dest);
uint8_t* getPipeRedirect();

// ---------------------------------------------------------------- vertices
struct HostVertex {
    float pos[3];
    float nrm[3];
    float bin[3];
    float tan[3];
    uint8_t clr[2][4];
    float tex[8][2];
    uint8_t mtx[12];  // pos matrix index, tex0..7 matrix index, 3 pad
    HostVertex() {}   // left uninitialised: the loader writes every field
};

enum PrimClass { PRIM_TRIS, PRIM_LINES, PRIM_POINTS };

// ---------------------------------------------------------------- renderer (gx_render.cpp)
void rendererInit(int efbScale);
void flushBatch();
// adds a primitive; vertices already decoded
// Packed vertex formats: a vertex holds only the attributes its GX vertex
// descriptor enables (position always); see vtxFmtLayout in gx_fifo.cpp.
enum VtxFmtBits : uint32_t {
    VF_NRM = 1u << 0,
    VF_NBT = 1u << 1,   // with VF_NRM: binormal and tangent follow the normal
    VF_CLR0 = 1u << 2,  // VF_CLR1 = VF_CLR0 << 1
    VF_TEX0 = 1u << 4,  // texcoord t: VF_TEX0 << t
    VF_MTX = 1u << 12,  // per-vertex matrix indices
};
struct VtxFmtLayout {
    uint16_t stride, nrm, clr[2], tex[8], mtx;  // byte offsets; position is at 0
};
const VtxFmtLayout& vtxFmtLayout(uint32_t fmt);
// Expands a packed vertex (for traces).
void unpackVertex(uint32_t fmt, const uint8_t* src, const uint8_t defMtx[9], HostVertex& out);

// The loader decodes a primitive's vertices straight into the batch:
// primitiveBegin returns room for `count` vertices of format `fmt` (after
// flushing when the primitive class or format changes), primitiveEnd adds
// the primitive's indices.
uint8_t* primitiveBegin(uint8_t opcode, uint32_t count, uint32_t fmt, uint32_t stride);
void primitiveEnd(uint8_t opcode, uint32_t count);
void onStateChange();  // called before any register write that changes state
void executeCopy(uint32_t execReg);
void markXfMemDirty();
uint32_t peekColor(int x, int y);  // A<<24|R<<16|G<<8|B
uint32_t peekZ(int x, int y);      // 24-bit
void pixMetricClear();
uint32_t pixMetricRead();

// ---------------------------------------------------------------- textures (gx_texture.cpp)
struct TexKey;
unsigned bindTextureMap(int map, float* outW, float* outH);  // returns GL name
void textureInvalidateAll();
void textureInvalidateRange(const void* p, uint32_t size);
void textureShutdown();
unsigned efbCopyLookup(const void* addr, int* w, int* h);
void efbCopyRegister(const void* addr, unsigned tex, int w, int h, uint32_t fmt);
void efbCopySetBytes(const void* addr, uint32_t bytes);
void textureCpuWrote(const void* p, uint32_t size);
uint32_t copyLayout(uint32_t copyFmt, bool z);
uint32_t encodeTexture(const uint8_t* rgba, uint32_t fmt, uint32_t w, uint32_t h, uint8_t* dst);
uint32_t texLevelBytes(uint32_t fmt, uint32_t w, uint32_t h);
// Texture packs (gx_hires.cpp)
bool hiresEnabled();
bool hiresLog();
std::string hiresName(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped, const uint8_t* tlut,
                      uint32_t tlutBytes, bool onlyIfPresent);
unsigned hiresTexture(const std::string& name, int unit, uint32_t gxW, uint32_t gxH, int* scale);
void hiresShutdown();
const char* hiresDumpDir();  // SMS_TEXTURE_DUMP, or null
void hiresDump(const std::string& name, const uint8_t* rgba, uint32_t w, uint32_t h);
void hiresEndFrame();  // once per display copy
void hiresPreload();   // drain resource requests on the GL thread before gameplay
void hiresPrefetchResource(const void* data, uint32_t size, const char* name);
uint32_t hiresUploadedCount();
struct HiresStats { size_t residentBytes, decodedBytes, pendingCount; uint32_t uploaded; };
HiresStats hiresStats();
uint64_t xxh64(const void* data, size_t len, uint64_t seed);

// ---------------------------------------------------------------- shaders (gx_shader.cpp)
// The uniform values a program was last given (uploadUniforms skips the ones
// that did not change; `init` is false until the first upload).
struct UniformCache {
    bool init;
    int tevreg[16], konst[16], alpharef[2];
    float texscale[16], texsize[16], fog[4], fogcolor[4], indmtx[24], efb[2], proj[8], vp[8], ambmat[16];
};
struct ShaderProgram {
    unsigned prog;
    int id;  // creation order; SMS_GX_DUMP_SHADERS=dir writes dir/prog<id>.vs/.fs
    int uTevReg, uKonst, uTexScale, uAlphaRef, uFog, uFogColor, uIndMtx, uTexSize, uEfb,
        uProj, uViewport, uAmbMat, uDstAlpha;
    mutable UniformCache uc;
};
bool shaderHasEarlyFragmentTests();
bool earlyZWritesRejected();
const ShaderProgram* shaderForCurrentState(bool depthOnly = false);

// ---------------------------------------------------------------- util
uint64_t hashBytes(const void* data, size_t n, uint64_t seed = 0);
[[noreturn]] void fatal(const char* fmt, ...);
void logmsg(const char* fmt, ...);

}  // namespace gx

#endif
