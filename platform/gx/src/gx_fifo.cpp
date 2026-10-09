// Register file, command-stream parser and vertex loader.
#include "gx_internal.h"
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <unordered_map>

namespace gx {

State g;
extern uint32_t g_traceLastVat;
void (*drawSyncCallback)(uint16_t) = nullptr;

// ------------------------------------------------------------------ utilities
double monoSeconds() {
#ifdef __APPLE__
    static double tick = 0;
    if (tick == 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        tick = double(tb.numer) / double(tb.denom) * 1e-9;
    }
    return double(mach_absolute_time()) * tick;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

void logmsg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[sms_gx] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[sms_gx] fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

uint64_t hashBytes(const void* data, size_t n, uint64_t seed) {
    // 64-bit multiply/xor-shift hash over 8-byte words; not cryptographic.
    // Four independent lanes over 32-byte blocks, so the multiplies overlap
    // instead of forming one dependency chain (textures are hashed each frame).
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed ^ (0x9E3779B97F4A7C15ull * (n + 1));
    if (n >= 32) {
        uint64_t a = h, b = h ^ 0x2545F4914F6CDD1Dull, c = h ^ 0x632BE59BD9B4E019ull, d = h ^ 0x85EBCA77C2B2AE63ull;
        do {
            uint64_t w[4];
            memcpy(w, p, 32);
            a = (a ^ w[0] * 0xBF58476D1CE4E5B9ull);
            b = (b ^ w[1] * 0xBF58476D1CE4E5B9ull);
            c = (c ^ w[2] * 0xBF58476D1CE4E5B9ull);
            d = (d ^ w[3] * 0xBF58476D1CE4E5B9ull);
            a = (a << 27 | a >> 37) * 0x94D049BB133111EBull;
            b = (b << 27 | b >> 37) * 0x94D049BB133111EBull;
            c = (c << 27 | c >> 37) * 0x94D049BB133111EBull;
            d = (d << 27 | d >> 37) * 0x94D049BB133111EBull;
            p += 32;
            n -= 32;
        } while (n >= 32);
        h = a ^ (b << 17 | b >> 47) ^ (c << 31 | c >> 33) ^ (d << 45 | d >> 19);
        h *= 0x94D049BB133111EBull;
    }
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h ^= w * 0xBF58476D1CE4E5B9ull;
        h = (h << 27 | h >> 37) * 0x94D049BB133111EBull;
        p += 8;
        n -= 8;
    }
    uint64_t w = 0;
    memcpy(&w, p, n);
    h ^= w * 0xBF58476D1CE4E5B9ull + n;
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

// Default window: MEM1 mapped at the GameCube's own cached base, which is what
// the unmodified OSCachedToPhysical macro (addr - 0x80000000) assumes.
static uint8_t* s_memBase = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(0x80000000u));
static uint32_t s_memSize = 0x01800000;

void* physToPtr(uint32_t phys) {
    phys &= 0x3FFFFFFF;
    if (!phys) return nullptr;
    return s_memBase + phys;
}

uint32_t ptrToPhys(const void* p) {
    if (!p) return 0;
    uintptr_t off = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s_memBase);
    return static_cast<uint32_t>(off);
}

// ------------------------------------------------------------------ memory write stamps
// Every write the GPU side is told about (cache flushes, DVD and ARAM
// transfers) stamps the 4 KiB pages it covers with a running clock, so a
// cached decode can tell cheaply whether its source bytes may have changed.
// The first 64 MiB from the memory window's base are tracked (MEM1); other
// memory always counts as written.
enum : uint32_t { kStampShift = 12, kStampPages = (64u << 20) >> kStampShift };
static std::atomic<uint32_t> s_pageStamp[kStampPages];
static std::atomic<uint32_t> s_writeClock{1};

void memoryWritten(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s_memBase);
    const uintptr_t span = uintptr_t(kStampPages) << kStampShift;
    if (!n || a >= span) return;
    uintptr_t last = (std::min<uintptr_t>(a + n, span) - 1) >> kStampShift;
    uint32_t stamp = s_writeClock.fetch_add(1, std::memory_order_relaxed) + 1;
    for (uintptr_t pg = a >> kStampShift; pg <= last; pg++) s_pageStamp[pg].store(stamp, std::memory_order_relaxed);
}

uint32_t writeClock() { return s_writeClock.load(std::memory_order_relaxed); }

// SMS_GX_HASH_ALWAYS=1: the caches compare their source bytes by hash on
// every use instead of trusting the write stamps between checks.
bool trustWriteStamps() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SMS_GX_HASH_ALWAYS");
        on = !(e && e[0] == '1');
    }
    return on != 0;
}

// True when no page of [p, p + n) was stamped after `since` (a writeClock()
// value); false for memory outside the tracked range.
bool unwrittenSince(const void* p, size_t n, uint32_t since) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s_memBase);
    const uintptr_t span = uintptr_t(kStampPages) << kStampShift;
    if (a >= span || a + n > span) return false;
    if (!n) return true;
    for (uintptr_t pg = a >> kStampShift, last = (a + n - 1) >> kStampShift; pg <= last; pg++)
        if (int32_t(s_pageStamp[pg].load(std::memory_order_relaxed) - since) > 0) return false;
    return true;
}

// ------------------------------------------------------------------ big-endian ranges
// Memory holding data in its on-disc byte order (resource files loaded in
// place).  Vertex arrays that start inside such a range are read big-endian.
static std::map<uintptr_t, uintptr_t> s_beRanges;  // start -> end
static uint32_t s_beGen = 1;                        // bumped when s_beRanges changes
static uintptr_t s_beHitLo = 1, s_beHitHi = 0;      // the range the last lookup found

bool isBigEndianData(const void* p) {
    if (s_beRanges.empty() || !p) return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a >= s_beHitLo && a < s_beHitHi) return true;  // array bases cluster in one resource
    auto it = s_beRanges.upper_bound(a);
    if (it == s_beRanges.begin()) return false;
    --it;
    if (a >= it->second) return false;
    s_beHitLo = it->first;
    s_beHitHi = it->second;
    return true;
}

static inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// ------------------------------------------------------------------ register writes
bool g_defaultArrayBE = false;
uint32_t g_arrayGen = 1;
#define s_defaultArrayBE g_defaultArrayBE

void resetState() {
    bool be[16];
    memcpy(be, g.arrayBigEndian, sizeof(be));
    memset(&g, 0, sizeof(g));
    for (int i = 0; i < 16; i++) g.arrayBigEndian[i] = s_defaultArrayBE;
    g_arrayGen++;
    g.bpMask = 0xFFFFFF;
    for (int i = 0; i < 256; i++) g.bp[i] = 0;
    // identity matrices where the SDK expects them: GX_IDENTITY (row 60) and
    // GX_PTIDENTITY (post matrix 61)
    const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; i++) {
        memcpy(&g.xfMem[60 * 4 + i], &ident[i], 4);
        memcpy(&g.xfMem[0x500 + 61 * 4 + i], &ident[i], 4);
    }
    (void)be;
}

void writeBP(uint32_t value) {
    uint32_t reg = value >> 24;
    uint32_t data = value & 0xFFFFFF;
    if (reg == BP_MASK) {
        g.bpMask = data;
        return;
    }
    if (g.bpMask != 0xFFFFFF) {
        data = (g.bp[reg] & ~g.bpMask) | (data & g.bpMask);
        g.bpMask = 0xFFFFFF;
    }
    if (reg >= BP_TEV_REG && reg < BP_TEV_REG + 8 && (data & 0x800000)) {
        if (g.kreg[reg & 7] != data) {
            onStateChange();
            g.kreg[reg & 7] = data;
        }
        return;
    }
    switch (reg) {
    case BP_COPY_EXEC:
        onStateChange();
        g.bp[reg] = data;
        executeCopy(data);
        return;
    case BP_PE_TOKEN_INT:
    case BP_PE_TOKEN:
        g.bp[reg] = data;
        g.drawSyncToken = uint16_t(data & 0xFFFF);
        if (reg == BP_PE_TOKEN_INT && drawSyncCallback) {
            flushBatch();
            drawSyncCallback(g.drawSyncToken);
        }
        return;
    case BP_DRAWDONE:
        flushBatch();
        g.bp[reg] = data;
        return;
    case BP_LOADTLUT1: {
        g.bp[reg] = data;
        uint32_t tmemOff = (data & 0x3FF) << 9;
        uint32_t bytes = ((data >> 10) & 0x7FF) * 32;
        const uint8_t* src = static_cast<const uint8_t*>(physToPtr(g.bp[BP_LOADTLUT0] << 5));
        if (tmemOff + bytes > TMEM_TLUT_SIZE) bytes = TMEM_TLUT_SIZE - tmemOff;
        onStateChange();
        if (src && bytes) memcpy(g.tlutMem + tmemOff, src, bytes);
        return;
    }
    case BP_TEXINVALIDATE:
        g.bp[reg] = data;
        textureInvalidateAll();
        return;
    default:
        break;
    }
    if (g.bp[reg] != data) {
        onStateChange();
        g.bp[reg] = data;
    }
    if (reg == BP_COPY_DST) g.copyDest = physToPtr(data << 5);
    if ((reg >= BP_TX_IMAGE3 && reg < BP_TX_IMAGE3 + 4) || (reg >= BP_TX_IMAGE3 + 0x20 && reg < BP_TX_IMAGE3 + 0x24)) {
        int map = (reg & 3) + ((reg & 0x20) ? 4 : 0);
        g.texImage[map] = static_cast<const uint8_t*>(physToPtr(data << 5));
    }
}

void writeCP(uint8_t reg, uint32_t value) {
    switch (reg & 0xF0) {
    case CP_MATIDX_A: g.cpMatA = value; break;
    case CP_MATIDX_B: g.cpMatB = value; break;
    case CP_VCD_LO: g.cpVcdLo = value; break;
    case CP_VCD_HI: g.cpVcdHi = value; break;
    case CP_VAT_A: g.cpVatA[reg & 7] = value; break;
    case CP_VAT_B: g.cpVatB[reg & 7] = value; break;
    case CP_VAT_C: g.cpVatC[reg & 7] = value; break;
    case CP_ARRAY_BASE: {
        // J3D sets every array again for each shape: unchanged ones keep the
        // vertex loader's setups (and skip the byte-order lookup)
        // (when nothing else changed the slot since this computed it)
        struct Last {
            const uint8_t* base;
            uint32_t beGen;
            bool def, be;
        };
        static Last s_last[16];
        const uint8_t* base = static_cast<const uint8_t*>(physToPtr(value));
        int slot = reg & 15;
        Last& l = s_last[slot];
        if (base == l.base && base == g.arrayBase[slot] && l.beGen == s_beGen && l.def == s_defaultArrayBE &&
            l.be == g.arrayBigEndian[slot])
            break;
        g.arrayBase[slot] = base;
        g.arrayBigEndian[slot] = s_defaultArrayBE || isBigEndianData(base);
        l = Last{base, s_beGen, s_defaultArrayBE, g.arrayBigEndian[slot]};
        g_arrayGen++;
        break;
    }
    case CP_ARRAY_STRIDE:
        if (g.arrayStride[reg & 15] == (value & 0xFF)) break;
        g.arrayStride[reg & 15] = value & 0xFF;
        g_arrayGen++;
        break;
    default: break;
    }
}

void writeXF(uint16_t addr, uint32_t count, const uint32_t* words) {
    for (uint32_t i = 0; i < count; i++, addr++) {
        uint32_t v = words[i];
        if (addr < XF_MEM_WORDS) {
            if (g.xfMem[addr] != v) {
                onStateChange();
                g.xfMem[addr] = v;
                markXfMemDirty();
            }
        } else if (addr >= XF_REG_BASE && addr < XF_REG_BASE + XF_REG_COUNT) {
            uint32_t r = addr - XF_REG_BASE;
            if (g.xfReg[r] != v) {
                onStateChange();
                g.xfReg[r] = v;
            }
        }
    }
}

void writeXFf(uint16_t addr, uint32_t count, const float* f) {
    uint32_t w[16];
    while (count) {
        uint32_t n = count > 16 ? 16 : count;
        memcpy(w, f, n * 4);
        writeXF(addr, n, w);
        addr += n;
        f += n;
        count -= n;
    }
}

// ------------------------------------------------------------------ vertex loader
struct AttrFmt {
    uint8_t mode;   // 0 none, 1 direct, 2 idx8, 3 idx16
    uint8_t cnt, type, frac;
};

struct VtxLayout {
    AttrFmt pnmtx;          // direct u8 only
    AttrFmt texmtx[8];
    AttrFmt pos, nrm, clr[2], tex[8];
    bool nbt, nbt3;
    uint32_t size;
    uint32_t gen;  // unique per built layout (layoutFor)
};

static const uint8_t kCompSize[5] = {1, 1, 2, 2, 4};
static const uint8_t kColorSize[6] = {2, 3, 4, 2, 3, 4};

static void buildLayout(int vat, VtxLayout& L) {
    uint32_t lo = g.cpVcdLo, hi = g.cpVcdHi;
    uint32_t A = g.cpVatA[vat], B = g.cpVatB[vat], C = g.cpVatC[vat];
    memset(&L, 0, sizeof(L));
    L.pnmtx.mode = lo & 1;
    for (int i = 0; i < 8; i++) L.texmtx[i].mode = (lo >> (1 + i)) & 1;
    L.pos = {uint8_t((lo >> 9) & 3), uint8_t(A & 1), uint8_t((A >> 1) & 7), uint8_t((A >> 4) & 31)};
    L.nrm = {uint8_t((lo >> 11) & 3), uint8_t((A >> 9) & 1), uint8_t((A >> 10) & 7), 0};
    L.clr[0] = {uint8_t((lo >> 13) & 3), uint8_t((A >> 13) & 1), uint8_t((A >> 14) & 7), 0};
    L.clr[1] = {uint8_t((lo >> 15) & 3), uint8_t((A >> 17) & 1), uint8_t((A >> 18) & 7), 0};
    L.tex[0] = {uint8_t(hi & 3), uint8_t((A >> 21) & 1), uint8_t((A >> 22) & 7), uint8_t((A >> 25) & 31)};
    L.tex[1] = {uint8_t((hi >> 2) & 3), uint8_t(B & 1), uint8_t((B >> 1) & 7), uint8_t((B >> 4) & 31)};
    L.tex[2] = {uint8_t((hi >> 4) & 3), uint8_t((B >> 9) & 1), uint8_t((B >> 10) & 7), uint8_t((B >> 13) & 31)};
    L.tex[3] = {uint8_t((hi >> 6) & 3), uint8_t((B >> 18) & 1), uint8_t((B >> 19) & 7), uint8_t((B >> 22) & 31)};
    L.tex[4] = {uint8_t((hi >> 8) & 3), uint8_t((B >> 27) & 1), uint8_t((B >> 28) & 7), uint8_t(C & 31)};
    L.tex[5] = {uint8_t((hi >> 10) & 3), uint8_t((C >> 5) & 1), uint8_t((C >> 6) & 7), uint8_t((C >> 9) & 31)};
    L.tex[6] = {uint8_t((hi >> 12) & 3), uint8_t((C >> 14) & 1), uint8_t((C >> 15) & 7), uint8_t((C >> 18) & 31)};
    L.tex[7] = {uint8_t((hi >> 14) & 3), uint8_t((C >> 23) & 1), uint8_t((C >> 24) & 7), uint8_t((C >> 27) & 31)};
    L.nbt = L.nrm.cnt == 1;
    L.nbt3 = L.nbt && (A >> 31) != 0;

    uint32_t s = 0;
    s += L.pnmtx.mode;
    for (int i = 0; i < 8; i++) s += L.texmtx[i].mode;
    auto idxSize = [](uint8_t mode) -> uint32_t { return mode == 2 ? 1 : mode == 3 ? 2 : 0; };
    auto comp = [](uint8_t type) -> uint32_t { return type < 5 ? kCompSize[type] : 4; };
    if (L.pos.mode == 1) s += comp(L.pos.type) * (L.pos.cnt ? 3 : 2);
    else s += idxSize(L.pos.mode);
    if (L.nrm.mode == 1) s += comp(L.nrm.type) * (L.nbt ? 9 : 3);
    else s += idxSize(L.nrm.mode) * (L.nbt3 ? 3 : 1);
    for (int c = 0; c < 2; c++) {
        if (L.clr[c].mode == 1) s += L.clr[c].type < 6 ? kColorSize[L.clr[c].type] : 4;
        else s += idxSize(L.clr[c].mode);
    }
    for (int t = 0; t < 8; t++) {
        if (L.tex[t].mode == 1) s += comp(L.tex[t].type) * (L.tex[t].cnt ? 2 : 1);
        else s += idxSize(L.tex[t].mode);
    }
    L.size = s;
}

// buildLayout for the current registers, rebuilt only when they change
static const VtxLayout& layoutFor(int vat) {
    struct Cached {
        uint32_t lo, hi, A, B, C;
        bool valid;
        VtxLayout L;
    };
    static Cached cache[8];
    static uint32_t s_gen = 0;
    Cached& c = cache[vat];
    if (!c.valid || c.lo != g.cpVcdLo || c.hi != g.cpVcdHi || c.A != g.cpVatA[vat] || c.B != g.cpVatB[vat] ||
        c.C != g.cpVatC[vat]) {
        c.lo = g.cpVcdLo;
        c.hi = g.cpVcdHi;
        c.A = g.cpVatA[vat];
        c.B = g.cpVatB[vat];
        c.C = g.cpVatC[vat];
        c.valid = true;
        buildLayout(vat, c.L);
        c.L.gen = ++s_gen;
    }
    return c.L;
}

// reads one component from p in the given byte order
static inline float readComp(const uint8_t* p, uint8_t type, float scale, bool be) {
    switch (type) {
    case 0: return p[0] * scale;
    case 1: return int8_t(p[0]) * scale;
    case 2: {
        uint16_t v = be ? be16(p) : uint16_t(p[0] | p[1] << 8);
        return v * scale;
    }
    case 3: {
        uint16_t v = be ? be16(p) : uint16_t(p[0] | p[1] << 8);
        return int16_t(v) * scale;
    }
    default: {
        uint32_t v = be ? be32(p) : (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    }
}

static inline void readColor(const uint8_t* p, uint8_t type, bool be, uint8_t out[4]) {
    switch (type) {
    case 0: {  // RGB565
        uint16_t v = be ? be16(p) : uint16_t(p[0] | p[1] << 8);
        out[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
        out[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
        out[2] = uint8_t((v & 31) * 255 / 31);
        out[3] = 255;
        break;
    }
    case 1:  // RGB8
    case 2:  // RGBX8
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
        break;
    case 3: {  // RGBA4
        uint16_t v = be ? be16(p) : uint16_t(p[0] | p[1] << 8);
        out[0] = uint8_t(((v >> 12) & 15) * 17);
        out[1] = uint8_t(((v >> 8) & 15) * 17);
        out[2] = uint8_t(((v >> 4) & 15) * 17);
        out[3] = uint8_t((v & 15) * 17);
        break;
    }
    case 4: {  // RGBA6
        uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
        if (!be) v = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        out[0] = uint8_t(((v >> 18) & 63) * 255 / 63);
        out[1] = uint8_t(((v >> 12) & 63) * 255 / 63);
        out[2] = uint8_t(((v >> 6) & 63) * 255 / 63);
        out[3] = uint8_t((v & 63) * 255 / 63);
        break;
    }
    default:  // RGBA8: bytes are R,G,B,A in memory order either way
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        break;
    }
}

// attribute array slot for CP array registers
enum { ARR_POS = 0, ARR_NRM = 1, ARR_CLR0 = 2, ARR_TEX0 = 4 };

static const uint8_t* arrayElem(int slot, uint32_t idx) {
    const uint8_t* base = g.arrayBase[slot];
    if (!base) return nullptr;
    return base + size_t(idx) * g.arrayStride[slot];
}

static uint32_t s_vertsLoaded = 0;
double g_decodeSeconds = 0;  // detailed timers: time in the vertex loader
extern double g_flushSeconds;

// ------------------------------------------------------------------ packed vertex formats
// A batch's vertices carry only the attributes the GX vertex descriptor
// enables (position always); the renderer feeds the others as constant
// vertex attributes. Layout: pos f32x3 | nrm f32x3 | bin, tan f32x3 (NBT) |
// clr0 u8x4 | clr1 u8x4 | tex0..7 f32x2 | matrix indices u8x12.
const VtxFmtLayout& vtxFmtLayout(uint32_t fmt) {
    static std::unordered_map<uint32_t, VtxFmtLayout> cache;
    auto it = cache.find(fmt);
    if (it != cache.end()) return it->second;
    VtxFmtLayout l;
    memset(&l, 0, sizeof(l));
    uint16_t o = 12;
    if (fmt & VF_NRM) {
        l.nrm = o;
        o += (fmt & VF_NBT) ? 36 : 12;
    }
    for (int c = 0; c < 2; c++)
        if (fmt & (VF_CLR0 << c)) {
            l.clr[c] = o;
            o += 4;
        }
    for (int t = 0; t < 8; t++)
        if (fmt & (VF_TEX0 << t)) {
            l.tex[t] = o;
            o += 8;
        }
    if (fmt & VF_MTX) {
        l.mtx = o;
        o += 12;
    }
    l.stride = o;
    return cache.emplace(fmt, l).first->second;
}

void unpackVertex(uint32_t fmt, const uint8_t* src, const uint8_t defMtx[9], HostVertex& hv) {
    const VtxFmtLayout& l = vtxFmtLayout(fmt);
    memset(static_cast<void*>(&hv), 0, sizeof(hv));
    hv.nrm[2] = 1.0f;
    memset(hv.clr, 255, sizeof(hv.clr));
    memcpy(hv.mtx, defMtx, 9);
    memcpy(hv.pos, src, 12);
    if (fmt & VF_NRM) memcpy(hv.nrm, src + l.nrm, 12);
    if (fmt & VF_NBT) {
        memcpy(hv.bin, src + l.nrm + 12, 12);
        memcpy(hv.tan, src + l.nrm + 24, 12);
    }
    for (int c = 0; c < 2; c++)
        if (fmt & (VF_CLR0 << c)) memcpy(hv.clr[c], src + l.clr[c], 4);
    for (int t = 0; t < 8; t++)
        if (fmt & (VF_TEX0 << t)) memcpy(hv.tex[t], src + l.tex[t], 8);
    if (fmt & VF_MTX) memcpy(hv.mtx, src + l.mtx, 12);
}

// ------------------------------------------------------------------ attribute readers
// One reader per attribute, chosen once per setup: MODE is 1 (inline),
// 2 (8-bit index) or 3 (16-bit index), T the component type, N the components
// in the data, NOUT the floats written (extra ones get the op's defaults), BE
// whether the data is big-endian (inline data always is). Every vertex of a
// primitive has the same size in the stream and each attribute the same place
// in it, so a reader loads its attribute for all of a primitive's vertices.
struct DecOp;
typedef void (*DecFn)(const DecOp& o, const uint8_t* in, uint32_t inStride, uint8_t* out, uint32_t outStride,
                      uint32_t count);
struct DecOp {
    DecFn fn;
    const uint8_t* base;  // array base (indexed modes)
    uint32_t stride;      // array stride
    float scale;          // 1 / 2^frac for integer components
    uint16_t src;         // byte offset in the stream's vertex
    uint16_t dst;         // byte offset in the packed vertex
    float def[9];         // written when an indexed element has no array
    uint8_t idxBytes;     // indexed modes: 1 or 2 (0 for inline data and matrix indices)
    uint8_t slot;         // indexed modes: the array
    uint16_t elemBytes;   // indexed modes: bytes one element takes in the array
};

template <typename T, bool BE> static inline float loadComp(const uint8_t* p, float scale);
template <> inline float loadComp<uint8_t, true>(const uint8_t* p, float s) { return float(p[0]) * s; }
template <> inline float loadComp<uint8_t, false>(const uint8_t* p, float s) { return float(p[0]) * s; }
template <> inline float loadComp<int8_t, true>(const uint8_t* p, float s) { return float(int8_t(p[0])) * s; }
template <> inline float loadComp<int8_t, false>(const uint8_t* p, float s) { return float(int8_t(p[0])) * s; }
template <> inline float loadComp<uint16_t, true>(const uint8_t* p, float s) { return float(uint16_t(p[0] << 8 | p[1])) * s; }
template <> inline float loadComp<uint16_t, false>(const uint8_t* p, float s) { return float(uint16_t(p[1] << 8 | p[0])) * s; }
template <> inline float loadComp<int16_t, true>(const uint8_t* p, float s) { return float(int16_t(p[0] << 8 | p[1])) * s; }
template <> inline float loadComp<int16_t, false>(const uint8_t* p, float s) { return float(int16_t(p[1] << 8 | p[0])) * s; }
template <> inline float loadComp<float, true>(const uint8_t* p, float) {
    uint32_t v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
template <> inline float loadComp<float, false>(const uint8_t* p, float) {
    float f;
    memcpy(&f, p, 4);
    return f;
}

// The attribute's data for the vertex at `in` (its place in the stream).
template <int MODE> static inline const uint8_t* fetch(const DecOp& o, const uint8_t* in) {
    if (MODE == 1) return in;
    uint32_t idx = MODE == 2 ? in[0] : uint32_t(in[0]) << 8 | in[1];
    return o.base + size_t(idx) * o.stride;
}

template <int MODE, typename T, int N, int NOUT, bool BE>
static void readVec(const DecOp& o, const uint8_t* in, uint32_t is, uint8_t* out, uint32_t os, uint32_t count) {
    in += o.src;
    out += o.dst;
    if (MODE != 1 && !o.base) {  // indexed, no array: the defaults
        for (uint32_t v = 0; v < count; v++, out += os) memcpy(out, o.def, NOUT * 4);
        return;
    }
    for (uint32_t v = 0; v < count; v++, in += is, out += os) {
        float* d = reinterpret_cast<float*>(out);
        const uint8_t* src = fetch<MODE>(o, in);
        for (int i = 0; i < N; i++) d[i] = loadComp<T, BE>(src + i * sizeof(T), o.scale);
        for (int i = N; i < NOUT; i++) d[i] = o.def[i];
    }
}

template <int MODE, int TYPE, bool BE> static inline void readClr1(const uint8_t* src, uint8_t* c) {
    switch (TYPE) {
    case 0: {  // RGB565
        uint16_t v = BE ? uint16_t(src[0] << 8 | src[1]) : uint16_t(src[1] << 8 | src[0]);
        c[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
        c[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
        c[2] = uint8_t((v & 31) * 255 / 31);
        c[3] = 255;
        break;
    }
    case 1:  // RGB8
    case 2:  // RGBX8
        c[0] = src[0];
        c[1] = src[1];
        c[2] = src[2];
        c[3] = 255;
        break;
    case 3: {  // RGBA4
        uint16_t v = BE ? uint16_t(src[0] << 8 | src[1]) : uint16_t(src[1] << 8 | src[0]);
        c[0] = uint8_t(((v >> 12) & 15) * 17);
        c[1] = uint8_t(((v >> 8) & 15) * 17);
        c[2] = uint8_t(((v >> 4) & 15) * 17);
        c[3] = uint8_t((v & 15) * 17);
        break;
    }
    case 4: {  // RGBA6
        uint32_t v = BE ? uint32_t(src[0]) << 16 | uint32_t(src[1]) << 8 | src[2]
                        : uint32_t(src[2]) << 16 | uint32_t(src[1]) << 8 | src[0];
        c[0] = uint8_t(((v >> 18) & 63) * 255 / 63);
        c[1] = uint8_t(((v >> 12) & 63) * 255 / 63);
        c[2] = uint8_t(((v >> 6) & 63) * 255 / 63);
        c[3] = uint8_t((v & 63) * 255 / 63);
        break;
    }
    default:  // RGBA8: bytes are R,G,B,A in memory order either way
        memcpy(c, src, 4);
        break;
    }
}

template <int MODE, int TYPE, bool BE>
static void readClr(const DecOp& o, const uint8_t* in, uint32_t is, uint8_t* out, uint32_t os, uint32_t count) {
    in += o.src;
    out += o.dst;
    if (MODE != 1 && !o.base) {
        for (uint32_t v = 0; v < count; v++, out += os) memset(out, 255, 4);
        return;
    }
    for (uint32_t v = 0; v < count; v++, in += is, out += os) readClr1<MODE, TYPE, BE>(fetch<MODE>(o, in), out);
}

// Picks readVec<MODE, T, N, NOUT, BE> for the runtime values.
template <int N, int NOUT> static DecFn pickVecN(uint8_t mode, uint8_t type, bool be) {
#define SMS_GX_VEC_T(M, B)                                                                         \
    switch (type) {                                                                                \
    case 0: return &readVec<M, uint8_t, N, NOUT, B>;                                               \
    case 1: return &readVec<M, int8_t, N, NOUT, B>;                                                \
    case 2: return &readVec<M, uint16_t, N, NOUT, B>;                                              \
    case 3: return &readVec<M, int16_t, N, NOUT, B>;                                               \
    default: return &readVec<M, float, N, NOUT, B>;                                                \
    }
    if (mode == 1) { SMS_GX_VEC_T(1, true) }
    if (mode == 2) {
        if (be) { SMS_GX_VEC_T(2, true) }
        SMS_GX_VEC_T(2, false)
    }
    if (be) { SMS_GX_VEC_T(3, true) }
    SMS_GX_VEC_T(3, false)
#undef SMS_GX_VEC_T
}

static DecFn pickClr(uint8_t mode, uint8_t type, bool be) {
#define SMS_GX_CLR_T(M, B)                                                                         \
    switch (type) {                                                                                \
    case 0: return &readClr<M, 0, B>;                                                              \
    case 1: return &readClr<M, 1, B>;                                                              \
    case 2: return &readClr<M, 2, B>;                                                              \
    case 3: return &readClr<M, 3, B>;                                                              \
    case 4: return &readClr<M, 4, B>;                                                              \
    default: return &readClr<M, 5, B>;                                                             \
    }
    if (mode == 1) { SMS_GX_CLR_T(1, true) }
    if (mode == 2) {
        if (be) { SMS_GX_CLR_T(2, true) }
        SMS_GX_CLR_T(2, false)
    }
    if (be) { SMS_GX_CLR_T(3, true) }
    SMS_GX_CLR_T(3, false)
#undef SMS_GX_CLR_T
}

// Per-vertex matrix indices (position/normal and texture matrices, inline
// u8 each): the defaults from the XF registers, overwritten by the present ones.
static uint8_t s_mtxDefault[12];
static uint16_t s_mtxPresent;  // bit k: slot k is in the stream
static void readMtx(const DecOp& o, const uint8_t* in, uint32_t is, uint8_t* out, uint32_t os, uint32_t count) {
    in += o.src;
    out += o.dst;
    for (uint32_t v = 0; v < count; v++, in += is, out += os) {
        memcpy(out, s_mtxDefault, 12);
        const uint8_t* q = in;
        for (int k = 0; k < 9; k++)
            if (s_mtxPresent & (1u << k)) out[k] = *q++ & 63;
    }
}

static void attrOp(DecOp& op, int slot, uint8_t mode, float scale, uint16_t src, uint16_t dst, uint16_t elemBytes) {
    op.base = mode >= 2 ? g.arrayBase[slot] : nullptr;
    op.stride = g.arrayStride[slot];
    op.scale = scale;
    op.src = src;
    op.dst = dst;
    memset(op.def, 0, sizeof(op.def));
    op.idxBytes = mode == 2 ? 1 : mode == 3 ? 2 : 0;
    op.slot = uint8_t(slot);
    op.elemBytes = elemBytes;
}

// A VAT's readers, packed format and matrix-index defaults, rebuilt only when
// its layout, the vertex arrays or the matrix-index registers change (a frame
// loads thousands of primitives through a handful of setups).
struct DecSetup {
    uint32_t layoutGen = 0, arrayGen = 0, matA = 0, matB = 0;
    uint32_t fmt, stride;
    uint16_t mtxPresent;
    uint8_t mtxDefault[12];
    int nops;
    DecOp ops[16];
};

static void buildSetup(DecSetup& S, const VtxLayout& L) {
    uint32_t matA = g.xfReg[XFR_MATIDX_A], matB = g.xfReg[XFR_MATIDX_B];
    S.layoutGen = L.gen;
    S.arrayGen = g_arrayGen;
    S.matA = matA;
    S.matB = matB;
    const uint8_t defMtx[12] = {
        uint8_t(matA & 63), uint8_t((matA >> 6) & 63), uint8_t((matA >> 12) & 63), uint8_t((matA >> 18) & 63),
        uint8_t((matA >> 24) & 63), uint8_t(matB & 63), uint8_t((matB >> 6) & 63), uint8_t((matB >> 12) & 63),
        uint8_t((matB >> 18) & 63), 0, 0, 0,
    };
    memcpy(S.mtxDefault, defMtx, 12);

    uint32_t fmt = 0;
    S.mtxPresent = L.pnmtx.mode ? 1 : 0;
    for (int i = 0; i < 8; i++)
        if (L.texmtx[i].mode) S.mtxPresent |= uint16_t(2u << i);
    if (S.mtxPresent) fmt |= VF_MTX;
    if (L.nrm.mode) fmt |= VF_NRM | (L.nbt ? VF_NBT : 0);
    for (int c = 0; c < 2; c++)
        if (L.clr[c].mode) fmt |= VF_CLR0 << c;
    for (int t = 0; t < 8; t++)
        if (L.tex[t].mode) fmt |= VF_TEX0 << t;
    const VtxFmtLayout& lay = vtxFmtLayout(fmt);
    S.fmt = fmt;
    S.stride = lay.stride;

    // ops in stream order: matrix indices, position, normal/NBT, colours,
    // texcoords; `at` is where each one's data starts in the stream's vertex
    auto idxSize = [](uint8_t mode) -> uint16_t { return mode == 2 ? 1 : mode == 3 ? 2 : 0; };
    auto comp = [](uint8_t type) -> uint16_t { return type < 5 ? kCompSize[type] : 4; };
    DecOp* ops = S.ops;
    int nops = 0;
    uint16_t at = 0;
    if (fmt & VF_MTX) {
        ops[nops].fn = &readMtx;
        ops[nops].src = 0;
        ops[nops].dst = lay.mtx;
        ops[nops].idxBytes = 0;
        nops++;
        for (int k = 0; k < 9; k++) at += (S.mtxPresent >> k) & 1;
    }
    if (L.pos.mode) {
        DecOp& op = ops[nops++];
        attrOp(op, ARR_POS, L.pos.mode, 1.0f / float(1u << L.pos.frac), at, 0,
               uint16_t(comp(L.pos.type) * (L.pos.cnt ? 3 : 2)));
        at += L.pos.mode == 1 ? uint16_t(comp(L.pos.type) * (L.pos.cnt ? 3 : 2)) : idxSize(L.pos.mode);
        bool be = L.pos.mode == 1 || g.arrayBigEndian[ARR_POS];
        op.fn = L.pos.cnt ? pickVecN<3, 3>(L.pos.mode, L.pos.type, be) : pickVecN<2, 3>(L.pos.mode, L.pos.type, be);
    }
    if (L.nrm.mode) {
        uint8_t t = L.nrm.type;
        float sc = t == 1 ? 1.0f / 64 : t == 3 ? 1.0f / 16384 : t == 0 ? 1.0f / 128 : t == 2 ? 1.0f / 32768 : 1.0f;
        bool be = L.nrm.mode == 1 || g.arrayBigEndian[ARR_NRM];
        if (L.nbt3 && L.nrm.mode != 1) {  // three indices, one per vector
            for (int k = 0; k < 3; k++) {
                DecOp& op = ops[nops++];
                attrOp(op, ARR_NRM, L.nrm.mode, sc, at, uint16_t(lay.nrm + 12 * k), uint16_t(comp(t) * 3));
                at += idxSize(L.nrm.mode);
                if (k == 0) op.def[2] = 1.0f;
                op.fn = pickVecN<3, 3>(L.nrm.mode, t, be);
            }
        } else {
            DecOp& op = ops[nops++];
            attrOp(op, ARR_NRM, L.nrm.mode, sc, at, lay.nrm, uint16_t(comp(t) * (L.nbt ? 9 : 3)));
            at += L.nrm.mode == 1 ? uint16_t(comp(t) * (L.nbt ? 9 : 3)) : idxSize(L.nrm.mode);
            op.def[2] = 1.0f;
            op.fn = L.nbt ? pickVecN<9, 9>(L.nrm.mode, t, be) : pickVecN<3, 3>(L.nrm.mode, t, be);
        }
    }
    for (int c = 0; c < 2; c++)
        if (L.clr[c].mode) {
            DecOp& op = ops[nops++];
            attrOp(op, ARR_CLR0 + c, L.clr[c].mode, 1.0f, at, lay.clr[c],
                   uint16_t(L.clr[c].type < 6 ? kColorSize[L.clr[c].type] : 4));
            at += L.clr[c].mode == 1 ? uint16_t(L.clr[c].type < 6 ? kColorSize[L.clr[c].type] : 4)
                                     : idxSize(L.clr[c].mode);
            op.fn = pickClr(L.clr[c].mode, L.clr[c].type, L.clr[c].mode == 1 || g.arrayBigEndian[ARR_CLR0 + c]);
        }
    for (int t = 0; t < 8; t++)
        if (L.tex[t].mode) {
            const AttrFmt& a = L.tex[t];
            DecOp& op = ops[nops++];
            attrOp(op, ARR_TEX0 + t, a.mode, 1.0f / float(1u << a.frac), at, lay.tex[t],
                   uint16_t(comp(a.type) * (a.cnt ? 2 : 1)));
            at += a.mode == 1 ? uint16_t(comp(a.type) * (a.cnt ? 2 : 1)) : idxSize(a.mode);
            bool be = a.mode == 1 || g.arrayBigEndian[ARR_TEX0 + t];
            op.fn = a.cnt ? pickVecN<2, 2>(a.mode, a.type, be) : pickVecN<1, 2>(a.mode, a.type, be);
        }
    S.nops = nops;
    if (at != L.size) fatal("vertex loader: attributes take %u of the vertex's %u bytes", at, L.size);
}

static void dlRecordPrimitive(uint8_t opcode, const uint8_t* p, uint32_t count, int vat, const DecSetup& S,
                              const uint8_t* out);
static struct DlEntry* s_dlRec = nullptr;  // the display list being recorded into the cache

static const uint8_t* decodeVertices(uint8_t opcode, const uint8_t* p, uint32_t count, const VtxLayout& L, int vat) {
    s_vertsLoaded += count;
    static DecSetup s_setup[8];
    DecSetup& S = s_setup[vat];
    if (S.layoutGen != L.gen || S.arrayGen != g_arrayGen || S.matA != g.xfReg[XFR_MATIDX_A] ||
        S.matB != g.xfReg[XFR_MATIDX_B])
        buildSetup(S, L);
    if (S.fmt & VF_MTX) {
        memcpy(s_mtxDefault, S.mtxDefault, 12);
        s_mtxPresent = S.mtxPresent;
    }

    const uint32_t stride = S.stride;
    const int nops = S.nops;
    const DecOp* ops = S.ops;
    uint8_t* out = primitiveBegin(opcode, count, S.fmt, stride);
    if (!L.pos.mode) memset(out, 0, size_t(count) * stride);  // no position: keep the slot defined
    for (int k = 0; k < nops; k++) ops[k].fn(ops[k], p, L.size, out, stride, count);
    primitiveEnd(opcode, count);
    if (s_dlRec) dlRecordPrimitive(opcode, p, count, vat, S, out);
    return p + size_t(count) * L.size;
}

// Commands 0x20/0x28/0x30/0x38: XF memory loaded from an element of array
// 12-15 (position, normal, texture matrices, lights).
static void loadIndexedXF(uint8_t op, uint32_t idx, uint32_t v) {
    uint32_t addr = v & 0xFFF, cnt = (v >> 12) + 1;
    int slot = 12 + ((op - 0x20) >> 3);
    const uint8_t* src = arrayElem(slot, idx);
    if (!src) return;
    uint32_t words[16];
    bool be = g.arrayBigEndian[slot];
    for (uint32_t k = 0; k < cnt; k++)
        words[k] = be ? be32(src + k * 4)
                      : (uint32_t(src[k * 4]) | uint32_t(src[k * 4 + 1]) << 8 | uint32_t(src[k * 4 + 2]) << 16 |
                         uint32_t(src[k * 4 + 3]) << 24);
    writeXF(uint16_t(addr), cnt, words);
}

// ------------------------------------------------------------------ command parser
static uint32_t s_dlDepth = 0;
static bool s_dlRecFail = false;  // the list being recorded does more than draw

// Executes complete commands from [p, p+n).  Returns bytes consumed; if the
// tail holds an incomplete command, *need receives its total length.
static uint32_t parse(const uint8_t* p, uint32_t n, uint32_t* need) {
    const uint8_t* start = p;
    const uint8_t* end = p + n;
    while (p < end) {
        uint8_t op = p[0];
        uint32_t avail = uint32_t(end - p);
        uint32_t len;
        if (op == 0x00 || op == 0x48 || op == 0x44) {  // NOP, invalidate vtx cache, unknown sync
            p++;
            continue;
        }
        if (s_dlRec && !(op >= 0x80 && op < 0xC0)) s_dlRecFail = true;
        if (op == 0x08) {
            len = 6;
            if (avail < len) break;
            writeCP(p[1], be32(p + 2));
        } else if (op == 0x10) {
            if (avail < 5) { len = 5; break; }
            uint32_t hdr = be32(p + 1);
            uint32_t cnt = (hdr >> 16) + 1;
            len = 5 + cnt * 4;
            if (avail < len) break;
            uint32_t words[64];
            uint16_t addr = uint16_t(hdr & 0xFFFF);
            for (uint32_t i = 0; i < cnt;) {
                uint32_t m = cnt - i > 64 ? 64 : cnt - i;
                for (uint32_t k = 0; k < m; k++) words[k] = be32(p + 5 + (i + k) * 4);
                writeXF(uint16_t(addr + i), m, words);
                i += m;
            }
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            len = 5;
            if (avail < len) break;
            loadIndexedXF(op, be16(p + 1), be16(p + 3));
        } else if (op == 0x40) {
            len = 9;
            if (avail < len) break;
            const uint8_t* dl = static_cast<const uint8_t*>(physToPtr(be32(p + 1)));
            uint32_t sz = be32(p + 5);
            if (s_dlDepth < 4 && dl) {
                s_dlDepth++;
                runCommands(dl, sz);
                s_dlDepth--;
            }
        } else if (op == 0x61) {
            len = 5;
            if (avail < len) break;
            writeBP(be32(p + 1));
        } else if (op >= 0x80 && op < 0xC0) {
            if (avail < 3) { len = 3; break; }
            const VtxLayout& L = layoutFor(op & 7);
            uint32_t cnt = be16(p + 1);
            len = 3 + cnt * L.size;
            if (avail < len) break;
            g_traceLastVat = op & 7;
            if (g_gxStats) {
                double f0 = g_flushSeconds;  // a batch flushed inside counts as drawing
                double t0 = monoSeconds();
                decodeVertices(op & 0xF8, p + 3, cnt, L, op & 7);
                g_decodeSeconds += monoSeconds() - t0 - (g_flushSeconds - f0);
            } else {
                decodeVertices(op & 0xF8, p + 3, cnt, L, op & 7);
            }
        } else {
            logmsg("unknown FIFO opcode 0x%02X, skipping byte", op);
            p++;
            continue;
        }
        p += len;
        continue;
    }
    if (p < end && need) {
        // recompute the length of the incomplete command for the caller
        uint8_t op = p[0];
        uint32_t avail = uint32_t(end - p);
        uint32_t len = 1;
        if (op == 0x08) len = 6;
        else if (op == 0x10) len = avail < 5 ? 5 : 5 + ((be32(p + 1) >> 16) + 1) * 4;
        else if (op >= 0x20 && op <= 0x38) len = 5;
        else if (op == 0x40) len = 9;
        else if (op == 0x61) len = 5;
        else if (op >= 0x80 && op < 0xC0) {
            if (avail < 3) len = 3;
            else {
                len = 3 + be16(p + 1) * layoutFor(op & 7).size;
            }
        }
        *need = len;
    }
    return uint32_t(p - start);
}

void runCommands(const uint8_t* data, uint32_t size) {
    uint32_t need = 0;
    uint32_t used = parse(data, size, &need);
    if (used < size) logmsg("display list ends inside a command (%u of %u bytes used)", used, size);
}

// ------------------------------------------------------------------ display-list cache
// Nearly all of a frame's vertices come from display lists the game calls
// again every frame (J3D shapes: draws with indexed attributes). The first
// call decodes a list as usual and keeps the packed vertices and batch
// indices it produced; later calls append those directly. A call reuses
// them when everything decoding read is unchanged: the vertex descriptor,
// the formats and arrays of the attributes it used, the default matrix
// indices (when vertices carry their own), the list's bytes and the bytes
// of every array element it indexes. Those bytes are compared by hash when
// a write stamp (a cache flush, DVD or ARAM transfer) touched their pages,
// and every few frames regardless, so CPU writes the game never flushed
// are picked up too. Lists that do anything but draw are not kept.
// SMS_GX_DL_CACHE=0 turns the cache off.
struct DlRun {  // consecutive primitives of one class and packed format
    PrimClass cls;
    uint32_t fmt, stride, nverts;
    std::vector<uint8_t> verts;
    std::vector<uint32_t> idx;  // relative to the run's first vertex
    ArenaRef arena;             // where it is stored for drawing in place
};
// A command of a list that only sets state (J3D's material and vertex-format
// lists), decoded once, so later calls skip the byte-level parsing.
struct StateOp {
    uint8_t type;  // 0x08 CP, 0x61 BP, 0x10 XF, 0x20-0x38 indexed XF load, 0x40 call
    uint8_t reg;   // CP register
    uint16_t a;    // XF: address; indexed load: index
    uint32_t v;    // CP/BP value; XF: first word in `words` (count in n); indexed load: its second half; call: size
    uint32_t n;    // XF: words; call: the list's physical address
};
struct DlArray {  // the part of an attribute array the list reads
    const uint8_t* base;
    uint32_t stride, lo, hi;  // bytes [lo, hi) from base
    uint64_t hash;
    uint8_t slot;
    bool be;
};
struct DlEntry {
    bool valid = false;
    bool stateOnly = false;  // no draws: replayed from `ops`
    uint32_t size = 0;
    uint64_t hash = 0;  // the list's bytes
    uint32_t vcdLo = 0, vcdHi = 0, matA = 0, matB = 0;
    uint32_t vat[8][3];
    uint8_t vatMask = 0, lastVat = 0;
    bool usesMtx = false;
    uint32_t verts = 0;
    size_t bytes = 0;
    std::vector<DlArray> arrays;
    std::vector<DlRun> runs;
    std::vector<StateOp> ops;    // a state-only list's commands, decoded
    std::vector<uint32_t> words; // their XF data
    uint32_t misses = 0;  // consecutive calls that could not reuse it
    uint32_t skip = 0;    // calls left to run without the cache
    uint32_t checkedClock = 0, checkedFrame = 0;  // when its source bytes were last compared
    bool hashAlways = false;  // its bytes once changed with no write reported: compared on every call
};
// The entries (stable addresses) and an open-addressing index of them by
// list address (a thousand lookups a frame).
static std::deque<DlEntry> s_dlEntries;
static std::vector<std::pair<const void*, DlEntry*>> s_dlIndex;
static size_t s_dlCacheBytes = 0;

static DlEntry& dlLookup(const void* key) {
    if (s_dlIndex.empty()) s_dlIndex.assign(4096, {nullptr, nullptr});
    size_t mask = s_dlIndex.size() - 1;
    size_t i = size_t((uint64_t(reinterpret_cast<uintptr_t>(key)) * 0x9E3779B97F4A7C15ull) >> 32) & mask;
    for (;; i = (i + 1) & mask) {
        if (s_dlIndex[i].first == key) return *s_dlIndex[i].second;
        if (!s_dlIndex[i].first) break;
    }
    if (s_dlEntries.size() * 2 >= s_dlIndex.size()) {  // grow, then insert into the new index
        std::vector<std::pair<const void*, DlEntry*>> old;
        old.swap(s_dlIndex);
        s_dlIndex.assign(old.size() * 2, {nullptr, nullptr});
        for (auto& kv : old)
            if (kv.first) {
                size_t m = s_dlIndex.size() - 1;
                size_t j = size_t((uint64_t(reinterpret_cast<uintptr_t>(kv.first)) * 0x9E3779B97F4A7C15ull) >> 32) & m;
                while (s_dlIndex[j].first) j = (j + 1) & m;
                s_dlIndex[j] = kv;
            }
        return dlLookup(key);
    }
    s_dlEntries.emplace_back();
    s_dlIndex[i] = {key, &s_dlEntries.back()};
    return s_dlEntries.back();
}

static void dlClear() {
    s_dlEntries.clear();
    s_dlIndex.clear();
    s_dlCacheBytes = 0;
}
static uint32_t s_dlLo[16], s_dlHi[16], s_dlElem[16];  // while recording: index range per array
FILE* traceFile();

// Lists are compared by hash at least this often (texture data: kRecheckFrames).
enum { kDlRecheckFrames = 30 };

static bool dlCacheEnabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SMS_GX_DL_CACHE");
        on = !(e && e[0] == '0');
    }
    return on != 0;
}

static void dlRecordPrimitive(uint8_t opcode, const uint8_t* p, uint32_t count, int vat, const DecSetup& S,
                              const uint8_t* out) {
    DlEntry& e = *s_dlRec;
    e.vatMask |= uint8_t(1u << vat);
    e.vat[vat][0] = g.cpVatA[vat];
    e.vat[vat][1] = g.cpVatB[vat];
    e.vat[vat][2] = g.cpVatC[vat];
    e.lastVat = uint8_t(vat);
    if (S.fmt & VF_MTX) e.usesMtx = true;
    PrimClass cls = primClass(opcode);
    if (e.runs.empty() || e.runs.back().cls != cls || e.runs.back().fmt != S.fmt)
        e.runs.push_back(DlRun{cls, S.fmt, S.stride, 0, {}, {}});
    DlRun& r = e.runs.back();
    r.verts.insert(r.verts.end(), out, out + size_t(count) * S.stride);
    size_t at = r.idx.size();
    r.idx.resize(at + size_t(count) * 3);
    r.idx.resize(at + primitiveIndices(opcode, count, r.nverts, r.idx.data() + at));
    r.nverts += count;
    e.verts += count;
    uint32_t vsize = layoutFor(vat).size;
    for (int k = 0; k < S.nops; k++) {
        const DecOp& o = S.ops[k];
        if (!o.idxBytes || !o.base) continue;
        uint32_t lo = s_dlLo[o.slot], hi = s_dlHi[o.slot];
        const uint8_t* q = p + o.src;
        for (uint32_t v = 0; v < count; v++, q += vsize) {
            uint32_t idx = o.idxBytes == 1 ? q[0] : uint32_t(q[0]) << 8 | q[1];
            if (idx < lo) lo = idx;
            if (idx > hi) hi = idx;
        }
        s_dlLo[o.slot] = lo;
        s_dlHi[o.slot] = hi;
        if (o.elemBytes > s_dlElem[o.slot]) s_dlElem[o.slot] = o.elemBytes;
    }
}

static bool dlReusable(DlEntry& e, const uint8_t* data, uint32_t size) {
    if (!e.valid || e.size != size) return false;
    if (!e.stateOnly && (e.vcdLo != g.cpVcdLo || e.vcdHi != g.cpVcdHi)) return false;
    for (int v = 0; v < 8; v++)
        if ((e.vatMask >> v) & 1)
            if (e.vat[v][0] != g.cpVatA[v] || e.vat[v][1] != g.cpVatB[v] || e.vat[v][2] != g.cpVatC[v]) return false;
    if (e.usesMtx && (e.matA != g.xfReg[XFR_MATIDX_A] || e.matB != g.xfReg[XFR_MATIDX_B])) return false;
    for (const DlArray& a : e.arrays)
        if (a.base != g.arrayBase[a.slot] || a.stride != g.arrayStride[a.slot] || a.be != g.arrayBigEndian[a.slot])
            return false;
    bool unwritten = trustWriteStamps() && !e.hashAlways && unwrittenSince(data, size, e.checkedClock);
    for (size_t i = 0; unwritten && i < e.arrays.size(); i++)
        unwritten = unwrittenSince(e.arrays[i].base + e.arrays[i].lo, e.arrays[i].hi - e.arrays[i].lo, e.checkedClock);
    if (unwritten && g_displayFrames - e.checkedFrame < kDlRecheckFrames) return true;
    uint32_t clock = writeClock();
    bool same = hashBytes(data, size) == e.hash;
    for (size_t i = 0; same && i < e.arrays.size(); i++)
        same = hashBytes(e.arrays[i].base + e.arrays[i].lo, e.arrays[i].hi - e.arrays[i].lo) == e.arrays[i].hash;
    if (!same) {
        if (unwritten) e.hashAlways = true;  // changed behind the write stamps' back
        return false;
    }
    e.checkedClock = clock;
    e.checkedFrame = g_displayFrames;
    return true;
}

// Decodes a list made only of state commands into e.ops; false if it draws
// (or ends inside a command).
static bool dlCompileState(DlEntry& e, const uint8_t* p, uint32_t size) {
    const uint8_t* end = p + size;
    while (p < end) {
        uint8_t op = p[0];
        uint32_t avail = uint32_t(end - p);
        if (op == 0x00 || op == 0x48 || op == 0x44) {
            p++;
            continue;
        }
        StateOp s{op, 0, 0, 0, 0};
        uint32_t len;
        if (op == 0x08) {
            len = 6;
            if (avail < len) return false;
            s.reg = p[1];
            s.v = be32(p + 2);
        } else if (op == 0x61) {
            len = 5;
            if (avail < len) return false;
            s.v = be32(p + 1);
        } else if (op == 0x10) {
            if (avail < 5) return false;
            uint32_t hdr = be32(p + 1), cnt = (hdr >> 16) + 1;
            len = 5 + cnt * 4;
            if (avail < len) return false;
            s.a = uint16_t(hdr & 0xFFFF);
            s.v = uint32_t(e.words.size());
            s.n = cnt;
            for (uint32_t i = 0; i < cnt; i++) e.words.push_back(be32(p + 5 + i * 4));
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            len = 5;
            if (avail < len) return false;
            s.a = be16(p + 1);
            s.v = be16(p + 3);
        } else if (op == 0x40) {
            len = 9;
            if (avail < len) return false;
            s.n = be32(p + 1);
            s.v = be32(p + 5);
        } else {
            return false;  // draws, or an unknown opcode
        }
        e.ops.push_back(s);
        p += len;
    }
    return true;
}

static void dlReplayState(const DlEntry& e) {
    for (const StateOp& s : e.ops) {
        switch (s.type) {
        case 0x08: writeCP(s.reg, s.v); break;
        case 0x61: writeBP(s.v); break;
        case 0x10:
            for (uint32_t i = 0; i < s.n;) {  // as parse: at most 64 words per writeXF
                uint32_t m = s.n - i > 64 ? 64 : s.n - i;
                writeXF(uint16_t(s.a + i), m, &e.words[s.v + i]);
                i += m;
            }
            break;
        case 0x40: {
            const uint8_t* dl = static_cast<const uint8_t*>(physToPtr(s.n));
            if (s_dlDepth < 4 && dl) {
                s_dlDepth++;
                runCommands(dl, s.v);
                s_dlDepth--;
            }
            break;
        }
        default: loadIndexedXF(s.type, s.a, s.v); break;
        }
    }
}

void callDisplayList(const uint8_t* data, uint32_t size) {
    if (!data || !dlCacheEnabled() || !rendererReady() || s_dlRec) {
        runCommands(data, size);
        return;
    }
    DlEntry& e = dlLookup(data);
    if (e.skip) {
        e.skip--;
        runCommands(data, size);
        return;
    }
    if (dlReusable(e, data, size)) {
        if (e.stateOnly) {
            dlReplayState(e);
            e.misses = 0;
            return;
        }
        const bool arena = arenaEnabled() && !traceFile();  // traces list the batch's own vertices
        for (DlRun& r : e.runs) {
            const uint32_t nidx = uint32_t(r.idx.size());
            if (arena && nidx &&
                (r.arena.gen == arenaGeneration() ||
                 arenaUpload(r.verts.data(), r.nverts, r.stride, r.idx.data(), nidx, r.arena)))
                appendArena(r.cls, r.fmt, r.stride, r.arena, r.nverts, nidx, r.verts.data());
            else
                appendDecoded(r.cls, r.fmt, r.stride, r.verts.data(), r.nverts, r.idx.data(), nidx);
        }
        s_vertsLoaded += e.verts;
        g_traceLastVat = e.lastVat;
        e.misses = 0;
        return;
    }
    // (re)record; a list that keeps changing is left alone for a while
    if (e.valid || e.size) e.misses++;
    if (batchUsesArena()) flushBatch();  // the batch may still read the runs about to be dropped
    s_dlCacheBytes -= e.bytes;
    uint32_t misses = e.misses;
    bool hashAlways = e.hashAlways;
    e = DlEntry();
    e.misses = misses;
    e.hashAlways = hashAlways;
    if (misses >= 3) e.skip = 4u << (misses - 3 < 8 ? misses - 3 : 8);
    if (s_dlCacheBytes > (size_t(96) << 20)) {  // keep the cache bounded: start over
        dlClear();
        runCommands(data, size);
        return;
    }
    e.size = size;
    e.vcdLo = g.cpVcdLo;
    e.vcdHi = g.cpVcdHi;
    e.matA = g.xfReg[XFR_MATIDX_A];
    e.matB = g.xfReg[XFR_MATIDX_B];
    for (int i = 0; i < 16; i++) {
        s_dlLo[i] = ~0u;
        s_dlHi[i] = 0;
        s_dlElem[i] = 0;
    }
    e.checkedClock = writeClock();
    e.checkedFrame = g_displayFrames;
    s_dlRec = &e;
    s_dlRecFail = false;
    uint32_t need = 0;
    uint32_t used = parse(data, size, &need);
    s_dlRec = nullptr;
    if (used < size) logmsg("display list ends inside a command (%u of %u bytes used)", used, size);
    if (s_dlRecFail || used < size) {  // not only draws: kept if it only sets state
        e.runs.clear();
        e.verts = 0;
        e.vatMask = 0;
        e.usesMtx = false;
        if (used < size || !dlCompileState(e, data, size)) {
            e.ops.clear();
            e.words.clear();
            e.skip = ~0u;
            return;
        }
        e.stateOnly = true;
        for (int i = 0; i < 16; i++) s_dlElem[i] = 0;  // arrays read by lists it called: not its own
    }
    e.hash = hashBytes(data, size);
    for (int i = 0; i < 16; i++) {
        if (!s_dlElem[i]) continue;
        DlArray a;
        a.base = g.arrayBase[i];
        a.stride = g.arrayStride[i];
        a.lo = s_dlLo[i] * a.stride;
        a.hi = s_dlHi[i] * a.stride + s_dlElem[i];
        a.hash = hashBytes(a.base + a.lo, a.hi - a.lo);
        a.slot = uint8_t(i);
        a.be = g.arrayBigEndian[i];
        e.arrays.push_back(a);
    }
    for (const DlRun& r : e.runs) e.bytes += r.verts.size() + r.idx.size() * 4;
    e.bytes += e.ops.size() * sizeof(StateOp) + e.words.size() * 4;
    s_dlCacheBytes += e.bytes;
    e.valid = true;
}

// ------------------------------------------------------------------ write-gather pipe
// Bytes written through the pipe collect here until they hold a whole
// command (s_pipeNeed bytes), which is then executed.
static uint32_t s_pipeCap = 0;
static uint32_t s_pipeNeed = 1;
static uint8_t* s_redirect = nullptr;
}  // namespace gx
// the buffer, its length and how far the inline writers may fill it (gx_pc.h)
extern "C" {
uint8_t* GXPC_PipeBuf = nullptr;
uint32_t GXPC_PipeLen = 0, GXPC_PipeFast = 0;
}
#define s_pipeBuf GXPC_PipeBuf
#define s_pipeLen GXPC_PipeLen
namespace gx {
static void pipeFastUpdate() { GXPC_PipeFast = s_redirect ? 0 : std::min(s_pipeCap, s_pipeNeed - 1); }

// Writes through a redirected pipe go to memory directly (uncached on the
// GameCube, so the game flushes nothing): stamp them when the redirect ends.
static uint8_t* s_redirectStart = nullptr;
void setPipeRedirect(uint8_t* dest) {
    if (s_redirectStart && s_redirect > s_redirectStart) memoryWritten(s_redirectStart, size_t(s_redirect - s_redirectStart));
    s_redirect = s_redirectStart = dest;
    pipeFastUpdate();
}
uint8_t* getPipeRedirect() { return s_redirect; }
bool pipeIdle() { return s_pipeLen == 0; }

static void pipeGrow(uint32_t need) {
    uint32_t cap = s_pipeCap ? s_pipeCap : 64u << 10;
    while (cap < need) cap *= 2;
    uint8_t* b = static_cast<uint8_t*>(realloc(s_pipeBuf, cap));
    if (!b) fatal("out of memory growing the write-gather pipe");
    s_pipeBuf = b;
    s_pipeCap = cap;
    pipeFastUpdate();
}

static void pipeRun() {
    uint32_t need = 1;
    uint32_t used = parse(s_pipeBuf, s_pipeLen, &need);
    if (used == s_pipeLen) {
        s_pipeLen = 0;
        s_pipeNeed = 1;
    } else {
        memmove(s_pipeBuf, s_pipeBuf + used, s_pipeLen - used);
        s_pipeLen -= used;
        s_pipeNeed = need;
    }
    pipeFastUpdate();
}

static inline void pipePut(const uint8_t* bytes, uint32_t n) {
    if (s_redirect) {
        memcpy(s_redirect, bytes, n);
        s_redirect += n;
        return;
    }
    if (s_pipeLen + n > s_pipeCap) pipeGrow(s_pipeLen + n);
    memcpy(s_pipeBuf + s_pipeLen, bytes, n);
    s_pipeLen += n;
    if (s_pipeLen >= s_pipeNeed) pipeRun();
}

void pipeWrite(const uint8_t* bytes, uint32_t n) { pipePut(bytes, n); }

}  // namespace gx

// ------------------------------------------------------------------ C entry points
using namespace gx;

extern "C" {

void GXPC_SetMemoryWindow(void* base, uint32_t size) {
    s_memBase = static_cast<uint8_t*>(base);
    s_memSize = size;
}

void GXPC_MemoryWritten(const void* p, uint32_t size) { memoryWritten(p, size); }

void GXPC_AddBigEndianRange(const void* p, uint32_t size) {
    if (!p || !size) return;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    s_beRanges[a] = a + size;
    s_beGen++;
    s_beHitLo = 1;
    s_beHitHi = 0;
}
void GXPC_RemoveBigEndianRange(const void* p) {
    s_beRanges.erase(reinterpret_cast<uintptr_t>(p));
    s_beGen++;
    s_beHitLo = 1;
    s_beHitHi = 0;
}
uint32_t GXPC_PtrToPhys(const void* p) { return ptrToPhys(p); }
void* GXPC_PhysToPtr(uint32_t phys) { return physToPtr(phys); }

void GXPC_SetArrayBigEndian(int attr, int bigEndian) {
    int slot;
    if (attr == 25) slot = 1;           // GX_VA_NBT shares the normal array
    else if (attr >= 9 && attr <= 24) slot = attr - 9;
    else return;
    g.arrayBigEndian[slot] = bigEndian != 0;
    g_arrayGen++;
}
void GXPC_SetDefaultArrayBigEndian(int bigEndian) {
    s_defaultArrayBE = bigEndian != 0;
    for (int i = 0; i < 16; i++) g.arrayBigEndian[i] = s_defaultArrayBE;
    g_arrayGen++;
}

void GXPC_Write8(uint8_t v) { pipePut(&v, 1); }
void GXPC_Write16(uint16_t v) {
    uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)};
    pipePut(b, 2);
}
void GXPC_Write32(uint32_t v) {
    uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    pipePut(b, 4);
}
void GXPC_WriteF32(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    GXPC_Write32(v);
}

}  // extern "C"

#include "sms_gx/gx_pc.h"
const GXPCWGPipe GXPC_WGPipe = {};
