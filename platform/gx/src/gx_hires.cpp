// Custom ("HD") textures in Dolphin's texture pack format.
//
// A pack is a folder of images named after the GX texture they replace:
//   tex1_<w>x<h>[_m]_<data hash>[_<palette hash>]_<format>.png
// <w>x<h> and <format> are the GX texture's size and format number, _m marks a
// texture sampled with mipmaps, and the hashes are XXH64 (seed 0) of the base
// level's bytes as they sit in memory and, for colour-indexed formats, of the
// palette entries the texture uses ("$" in place of the palette hash matches
// any palette). A file ending in _mip<n> is level n of the texture named by
// the rest; one ending in _arb has hand-made ("arbitrary") mipmaps, so its
// levels are not regenerated. Every pack made for Dolphin's "Load Custom
// Textures" works unchanged (the folder is usually named after the game ID,
// GMS or GMSE01), since it depends only on the texture data the game loads.
//
// Packs are found under the directories in SMS_TEXTURE_PACKS (separated by ':'
// or ';'), else under mods/textures/ in the working directory (or two levels
// up, when started from build/<os>-<arch>/). SMS_TEXTURE_PACKS=0 disables
// them. Resource textures prepare on a worker, then upload before gameplay.
// Late textures show their original until a bounded frame-end upload finishes.
#include "gx_internal.h"
#include "gl_funcs.h"
#include "gx_glcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO_OVERRIDE
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third_party/stb_image.h"
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include <unordered_set>

namespace gx {

// ------------------------------------------------------------------ XXH64
// From the xxHash specification (Yann Collet): 64-bit lanes over 32-byte
// stripes, then the tail and the avalanche.
static const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                      P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
static inline uint64_t rotl(uint64_t x, int r) { return x << r | x >> (64 - r); }
static inline uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static inline uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        do {
            v1 = xround(v1, rd64(p));
            v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16));
            v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (end - p >= 32);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(h, v1);
        h = xmerge(h, v2);
        h = xmerge(h, v3);
        h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += uint64_t(len);
    while (end - p >= 8) {
        h ^= xround(0, rd64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= uint64_t(rd32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= uint64_t(*p++) * P5;
        h = rotl(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

// ------------------------------------------------------------------ the index
struct PackFile {
    std::string path;               // level 0
    std::vector<std::string> mips;  // explicit levels 1.. (_mip<n>), may have gaps
    bool arbitrary = false;
};
static std::unordered_map<std::string, PackFile>& s_index =
    *new std::unordered_map<std::string, PackFile>;  // key: the texture name (read by the worker; never destroyed)
static std::atomic<int> s_state{-1};                       // published after the index and GL formats are ready
static uint32_t s_uploaded = 0;                            // replacements in GL

static bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static void indexFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(tolower(c));
    if (ext != ".png" && ext != ".dds") return;
    std::string stem = p.stem().string();
    if (stem.compare(0, 5, "tex1_") != 0) return;
    bool arb = false;
    if (endsWith(stem, "_arb")) {
        arb = true;
        stem.resize(stem.size() - 4);
    }
    int level = 0;
    size_t m = stem.rfind("_mip");
    if (m != std::string::npos && m + 4 < stem.size() &&
        stem.find_first_not_of("0123456789", m + 4) == std::string::npos) {
        level = atoi(stem.c_str() + m + 4);
        stem.resize(m);
    }
    PackFile& f = s_index[stem];
    if (level == 0) {
        if (f.path.empty()) f.path = p.string();
        f.arbitrary = f.arbitrary || arb;
    } else {
        if (f.mips.size() < size_t(level)) f.mips.resize(size_t(level));
        if (f.mips[size_t(level) - 1].empty()) f.mips[size_t(level) - 1] = p.string();
    }
}

static void queryFormats();
static void startWorker();

static void scan() {
    s_state = 0;
    std::vector<std::string> dirs;
    const char* e = getenv("SMS_TEXTURE_PACKS");
    if (e && strcmp(e, "0") == 0) return;
    if (e && *e) {
        std::string all = e, cur;
        for (char c : all + ";") {
            if (c == ';' || (c == ':' && !(cur.size() == 1 && isalpha(uint8_t(cur[0]))))) {
                if (!cur.empty()) dirs.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    } else {
        dirs = {"mods/textures", "../../mods/textures"};
    }
    namespace fs = std::filesystem;
    for (const std::string& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        size_t before = s_index.size();
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::follow_directory_symlink, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) indexFile(it->path());
        logmsg("texture pack: %zu textures under %s", s_index.size() - before, d.c_str());
    }
    for (auto it = s_index.begin(); it != s_index.end();)  // mips without a level 0
        it = it->second.path.empty() ? s_index.erase(it) : std::next(it);
    if (!s_index.empty()) {
        queryFormats();
        startWorker();
        s_state = 1;
    }
}

bool hiresEnabled() {
    if (s_state < 0) scan();
    return s_state == 1;
}

// SMS_TEXTURE_PACK_LOG=1 logs the pack name of every texture the game loads,
// and whether the pack replaced it (for checking or making a pack).
bool hiresLog() {
    static int on = -1;
    if (on < 0) on = getenv("SMS_TEXTURE_PACK_LOG") && atoi(getenv("SMS_TEXTURE_PACK_LOG")) != 0;
    return on != 0;
}

// SMS_TEXTURE_DUMP=dir: every texture the game loads is written there once,
// as tex1_<...>.png under its pack name (level 0, decoded), the way packs are
// made: dump, edit or upscale, keep the name.
const char* hiresDumpDir() {
    static const char* dir = nullptr;
    static bool checked = false;
    if (!checked) {
        checked = true;
        const char* e = getenv("SMS_TEXTURE_DUMP");
        if (e && *e && strcmp(e, "0") != 0) {
            dir = e;
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
    }
    return dir;
}

void hiresDump(const std::string& name, const uint8_t* rgba, uint32_t w, uint32_t h) {
    static std::unordered_set<std::string> done;
    const char* dir = hiresDumpDir();
    if (!dir || !done.insert(name).second) return;
    std::string path = std::string(dir) + "/" + name + ".png";
    if (!stbi_write_png(path.c_str(), int(w), int(h), 4, rgba, int(w) * 4))
        logmsg("texture dump: cannot write %s", path.c_str());
}

// ------------------------------------------------------------------ names
// The palette entries a colour-indexed texture's base level uses.
static void paletteRange(const uint8_t* data, size_t bytes, uint32_t fmt, uint32_t* lo, uint32_t* hi) {
    uint32_t mn = 0xFFFF, mx = 0;
    if (fmt == 8) {  // C4: two indices per byte
        for (size_t i = 0; i < bytes; i++) {
            uint32_t a = data[i] & 15, b = data[i] >> 4;
            mn = std::min(mn, std::min(a, b));
            mx = std::max(mx, std::max(a, b));
        }
    } else if (fmt == 9) {  // C8
        for (size_t i = 0; i < bytes; i++) {
            mn = std::min<uint32_t>(mn, data[i]);
            mx = std::max<uint32_t>(mx, data[i]);
        }
    } else {  // C14X2: big-endian halfwords
        for (size_t i = 0; i + 1 < bytes; i += 2) {
            uint32_t v = (uint32_t(data[i]) << 8 | data[i + 1]) & 0x3FFF;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
    }
    if (mn > mx) mn = mx = 0;
    *lo = mn;
    *hi = mx;
}

std::string hiresName(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped, const uint8_t* tlut,
                      uint32_t tlutBytes, bool onlyIfPresent) {
    size_t bytes = texLevelBytes(fmt, w, h);
    char base[96];
    snprintf(base, sizeof base, "tex1_%ux%u%s_%016llx", w, h, mipmapped ? "_m" : "",
             (unsigned long long)xxh64(data, bytes, 0));
    char fmtPart[8];
    snprintf(fmtPart, sizeof fmtPart, "_%u", fmt);
    bool ci = fmt == 8 || fmt == 9 || fmt == 10;
    if (ci && tlut) {
        uint32_t lo, hi;
        paletteRange(data, bytes, fmt, &lo, &hi);
        uint32_t off = 2 * lo, len = 2 * (hi + 1 - lo);
        if (off + len > tlutBytes) len = off < tlutBytes ? tlutBytes - off : 0;
        char tl[24];
        snprintf(tl, sizeof tl, "_%016llx", (unsigned long long)xxh64(tlut + off, len, 0));
        std::string full = std::string(base) + tl + fmtPart;
        if (!onlyIfPresent || s_index.count(full)) return full;
        std::string wild = std::string(base) + "_$" + fmtPart;
        if (s_index.count(wild)) return wild;
    }
    std::string plain = std::string(base) + fmtPart;
    if (!onlyIfPresent || s_index.count(plain)) return plain;
    return std::string();
}

// ------------------------------------------------------------------ loading
struct Loaded {
    int w = 0, h = 0;
    std::vector<std::vector<uint8_t>> levels;  // levels[0] always present
    GLenum compressed = 0;                     // the levels' GL block format, 0 for RGBA8
    int blocks = 0;                            // and their DDS block format (DDS_BC*)
    int first = 0;                             // the level levels[0] is (a completion's)
    bool arbitrary = false;
    bool failed = false;
};

// What the GL takes compressed (queried on the render thread by the scan).
static bool s_s3tc = false, s_bptc = false;

static void queryFormats() {
    GLint n = 0, major = 0, minor = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    s_bptc = major > 4 || (major == 4 && minor >= 2);  // core since 4.2
    for (GLint i = 0; i < n; i++) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (!e) continue;
        if (!strcmp(e, "GL_EXT_texture_compression_s3tc")) s_s3tc = true;
        if (!strcmp(e, "GL_ARB_texture_compression_bptc")) s_bptc = true;
    }
}

// DDS (the format most Dolphin packs ship in): BC1-3 and BC7 blocks with the
// file's own mip levels, or 32-bit RGBA/BGRA. Blocks the GL cannot take are
// decoded here; the levels a file leaves out are made by completeChain.
enum { DDS_BC1 = BLOCK_BC1, DDS_BC2 = BLOCK_BC2, DDS_BC3 = BLOCK_BC3, DDS_BC7 = BLOCK_BC7, DDS_RGBA, DDS_BGRA };

static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

static bool decodeDds(const std::string& path, Loaded* L) {
    std::vector<uint8_t> file;
    if (!readFile(path, file) || file.size() < 128 || memcmp(file.data(), "DDS ", 4) != 0) return false;
    const uint8_t* d = file.data();
    int h = int(rd32(d + 12)), w = int(rd32(d + 16));
    int mips = std::max(1, int(rd32(d + 28)));
    uint32_t pfFlags = rd32(d + 80), rmask = rd32(d + 92);
    size_t off = 128;
    int fmt = 0;
    if (!memcmp(d + 84, "DXT1", 4)) fmt = DDS_BC1;
    else if (!memcmp(d + 84, "DXT3", 4)) fmt = DDS_BC2;
    else if (!memcmp(d + 84, "DXT5", 4)) fmt = DDS_BC3;
    else if (!memcmp(d + 84, "DX10", 4) && file.size() >= 148) {
        uint32_t dxgi = rd32(d + 128);
        off = 148;
        if (dxgi >= 70 && dxgi <= 72) fmt = DDS_BC1;
        else if (dxgi >= 73 && dxgi <= 75) fmt = DDS_BC2;
        else if (dxgi >= 76 && dxgi <= 78) fmt = DDS_BC3;
        else if (dxgi >= 97 && dxgi <= 99) fmt = DDS_BC7;
        else if (dxgi == 28 || dxgi == 29) fmt = DDS_RGBA;
        else if (dxgi == 87 || dxgi == 91) fmt = DDS_BGRA;
    } else if ((pfFlags & 0x40) && rd32(d + 88) == 32) {
        fmt = rmask == 0x000000FFu ? DDS_RGBA : DDS_BGRA;
    }
    if (!fmt || w <= 0 || h <= 0) return false;
    bool block = fmt <= DDS_BC7;
    size_t bsz = fmt == DDS_BC1 ? 8 : 16;
    int full = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) full++;
    // Supported blocks stay compressed whatever levels the file has: the
    // missing ones are encoded in the same format (completeChain).
    bool gpu = (fmt <= DDS_BC3 && s_s3tc) || (fmt == DDS_BC7 && s_bptc);
    static const GLenum kGl[] = {0, 0x83F1, 0x83F2, 0x83F3, 0x8E8C};  // S3TC DXT1/3/5 RGBA, BPTC UNORM
    L->w = w;
    L->h = h;
    L->compressed = gpu ? kGl[fmt] : 0;
    L->blocks = gpu ? fmt : 0;
    int lw = w, lh = h;
    for (int m = 0; m < std::min(mips, full); m++) {
        int bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        size_t bytes = block ? size_t(bw) * bh * bsz : size_t(lw) * lh * 4;
        if (off + bytes > file.size()) break;
        const uint8_t* src = d + off;
        if (gpu) {
            L->levels.emplace_back(src, src + bytes);
        } else if (block) {  // decode whole blocks into a padded image, then crop
            std::vector<uint8_t> pad(size_t(bw) * 4 * bh * 4 * 4);
            int pitch = bw * 4 * 4;
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++) {
                    const uint8_t* b = src + (size_t(by) * bw + bx) * bsz;
                    uint8_t* o = pad.data() + size_t(by) * 4 * pitch + size_t(bx) * 16;
                    if (fmt == DDS_BC1) bcdec_bc1(b, o, pitch);
                    else if (fmt == DDS_BC2) bcdec_bc2(b, o, pitch);
                    else if (fmt == DDS_BC3) bcdec_bc3(b, o, pitch);
                    else bcdec_bc7(b, o, pitch);
                }
            std::vector<uint8_t> px(size_t(lw) * lh * 4);
            for (int y = 0; y < lh; y++) memcpy(&px[size_t(y) * lw * 4], &pad[size_t(y) * pitch], size_t(lw) * 4);
            L->levels.push_back(std::move(px));
        } else {
            std::vector<uint8_t> px(src, src + bytes);
            if (fmt == DDS_BGRA)
                for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
            L->levels.push_back(std::move(px));
        }
        off += bytes;
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }
    return !L->levels.empty();
}

// Each replacement is decoded once, on a worker thread, uploaded into its own
// GL texture on the render thread and its pixels freed; every cache entry
// whose data has that name samples the one texture.
struct Replacement {
    enum { QUEUED, READY, FAILED } state = QUEUED;
    GLuint tex = 0;
    int scale = 0;       // log2 of its size over the GX size
    size_t bytes = 0;    // in GL
    int w = 0, h = 0;    // its level 0
    int levels = 0;      // in GL; the rest of a compressed chain follows (completeLater)
    uint32_t used = 0;   // the display frame it was last sampled in
};
static size_t s_bytes = 0;     // all READY replacements
static uint32_t s_frame = 0;   // display frames (hiresEndFrame)
static std::unordered_map<std::string, Replacement> s_repl;  // render thread only
// Never destroyed: the worker is still waiting on s_cv when the process
// exits, and destroying a condition variable with a waiter blocks forever.
static std::mutex& s_mu = *new std::mutex;
static std::condition_variable& s_cv = *new std::condition_variable;
struct Request { std::string name; uint32_t w, h; bool rest = false; };  // rest: completeLater's levels
struct Decoded { Request request; std::unique_ptr<Loaded> image; size_t bytes; };
static std::deque<Request>& s_queue = *new std::deque<Request>;
static std::deque<Request>& s_restQueue = *new std::deque<Request>;  // after s_queue
static std::deque<Decoded>& s_decoded = *new std::deque<Decoded>;
static std::unordered_set<std::string>& s_requested = *new std::unordered_set<std::string>;
static size_t s_decodedBytes = 0;
static bool s_stop = false, s_busy = false, s_busyRest = false;
static std::thread* s_worker = nullptr;

static bool syncLoading() {
    static const bool sync = getenv("SMS_TEXTURE_PACK_SYNC") && atoi(getenv("SMS_TEXTURE_PACK_SYNC")) != 0;
    return sync;
}

static size_t pendingBudget() {
    static const size_t budget = [] {
        const char* e = getenv("SMS_TEXTURE_PACK_PENDING_MB");
        return size_t(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }();
    return budget;
}

static void requestTexture(const std::string& name, uint32_t w, uint32_t h) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_requested.insert(name).second) {
        s_queue.push_back({name, w, h});
        s_cv.notify_all();
    }
}

static bool preloading() {
    static const bool on = !getenv("SMS_TEXTURE_PACK_PRELOAD") || atoi(getenv("SMS_TEXTURE_PACK_PRELOAD")) != 0;
    return on && !syncLoading();
}

// Inspect resources before endian conversion. Only names/hashes are retained:
// the loader may free or reuse its buffer immediately after this returns.
static uint32_t readBe32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static uint32_t readBe16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

static void prefetchTimg(const uint8_t* data, size_t size, size_t off) {
    if (off > size || size - off < 0x20) return;
    const uint8_t* t = data + off;
    uint32_t fmt = t[0], w = readBe16(t + 2), h = readBe16(t + 4);
    if (fmt > 14 || !w || !h || w > 1024 || h > 1024) return;
    uint32_t image = readBe32(t + 0x1C);
    size_t bytes = texLevelBytes(fmt, w, h);
    if (image < 0x20 || image > size - off || !bytes || bytes > size - off - image) return;
    const uint8_t* palette = nullptr;
    uint32_t paletteBytes = 0;
    if (fmt == 8 || fmt == 9 || fmt == 10) {
        uint32_t pal = readBe32(t + 0x0C);
        paletteBytes = readBe16(t + 0x0A) * 2;
        if (!paletteBytes || pal < 0x20 || pal > size - off || paletteBytes > size - off - pal) return;
        palette = t + pal;
    }
    bool mipmapped = t[0x14] >= 2 && t[0x17] != 0;
    std::string name = hiresName(t + image, fmt, w, h, mipmapped, palette, paletteBytes, true);
    if (name.empty()) name = hiresName(t + image, fmt, w, h, !mipmapped, palette, paletteBytes, true);
    requestTexture(name, w, h);
}

void hiresPrefetchResource(const void* ptr, uint32_t size, const char* name) {
    // Never scan directories or query GL on a resource-loading thread.
    if (s_state != 1 || !preloading() || !ptr || size < 0x20) return;
    const uint8_t* data = static_cast<const uint8_t*>(ptr);
    bool model = !memcmp(data, "J3D2", 4), particle = !memcmp(data, "JEFFjpa1", 8);
    if (!model && !particle) {
        if (name) {
            std::string ext = std::filesystem::path(name).extension().string();
            for (char& c : ext) c = char(tolower(uint8_t(c)));
            if (ext == ".bti" && data[0x19] != 0x6E) prefetchTimg(data, size, 0);
        }
        return;
    }
    uint32_t blocks = readBe32(data + 0x0C);
    size_t pos = 0x20;
    for (uint32_t i = 0; i < blocks && pos <= size && size - pos >= 8; ++i) {
        uint32_t len = readBe32(data + pos + 4);
        if (len < 8 || len > size - pos) break;
        const uint8_t* b = data + pos;
        if (!memcmp(b, "TEX1", 4)) {
            if (particle) {
                prefetchTimg(b, len, 0x20);
            } else if (len >= 0x14) {
                uint32_t count = readBe16(b + 8), headers = readBe32(b + 0x0C);
                if (headers >= 0x14 && headers <= len && count <= (len - headers) / 0x20)
                    for (uint32_t j = 0; j < count; ++j) prefetchTimg(b, len, headers + size_t(j) * 0x20);
            }
        }
        pos += len;
    }
}

// The next level of a compressed one (lw x lh), by the same 2x2 box filter,
// in the same block format. Two block rows are decoded at a time, so even a
// 16384x16384 level never sits in memory as RGBA, and a large level's block
// rows are shared between threads (an 8192x8192 BC7 file takes about a second
// on four), leaving two cores to the game and render threads it runs beside.
static std::vector<uint8_t> nextBlockLevel(int fmt, const std::vector<uint8_t>& src, int lw, int lh) {
    int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
    int sbw = (lw + 3) / 4, sbh = (lh + 3) / 4, dbw = (nw + 3) / 4, dbh = (nh + 3) / 4;
    size_t bsz = fmt == DDS_BC1 ? 8 : 16;
    std::vector<uint8_t> dst(size_t(dbw) * dbh * bsz);
    int pitch = sbw * 16;  // bytes in a decoded source row
    auto rows = [&](int from, int to) {
        std::vector<uint8_t> in(size_t(pitch) * 8), out(size_t(dbw) * 16 * 4);
        for (int by = from; by < to; by++) {
            for (int k = 0; k < 2 && 2 * by + k < sbh; k++)  // source rows 8*by to 8*by+7
                for (int bx = 0; bx < sbw; bx++) {
                    const uint8_t* b = src.data() + (size_t(2 * by + k) * sbw + bx) * bsz;
                    uint8_t* o = in.data() + size_t(k) * 4 * pitch + size_t(bx) * 16;
                    if (fmt == DDS_BC1) bcdec_bc1(b, o, pitch);
                    else if (fmt == DDS_BC2) bcdec_bc2(b, o, pitch);
                    else if (fmt == DDS_BC3) bcdec_bc3(b, o, pitch);
                    else bcdec_bc7(b, o, pitch);
                }
            for (int r = 0; r < 4; r++) {  // this block row's 4 output rows; past the image, the last one again
                int y = std::min(4 * by + r, nh - 1);
                const uint8_t* s0 = &in[size_t(std::min(2 * y, lh - 1) - 8 * by) * pitch];
                const uint8_t* s1 = &in[size_t(std::min(2 * y + 1, lh - 1) - 8 * by) * pitch];
                uint8_t* o = &out[size_t(r) * dbw * 16];
                for (int x = 0; x < dbw * 4; x++) {
                    int xx = std::min(x, nw - 1);
                    int x0 = std::min(2 * xx, lw - 1) * 4, x1 = std::min(2 * xx + 1, lw - 1) * 4;
                    for (int c = 0; c < 4; c++)
                        o[x * 4 + c] = uint8_t((s0[x0 + c] + s0[x1 + c] + s1[x0 + c] + s1[x1 + c] + 2) / 4);
                }
            }
            for (int bx = 0; bx < dbw; bx++) {
                uint8_t block[64];
                for (int r = 0; r < 4; r++) memcpy(block + r * 16, &out[(size_t(r) * dbw * 4 + bx * 4) * 4], 16);
                encodeBlock(fmt, block, dst.data() + (size_t(by) * dbw + bx) * bsz);
            }
        }
    };
    int threads = std::min<int>({int(std::thread::hardware_concurrency()) - 2, 8, dbh / 16});
    if (threads <= 1) {
        rows(0, dbh);
        return dst;
    }
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; t++) pool.emplace_back(rows, dbh * t / threads, dbh * (t + 1) / threads);
    rows(0, dbh / threads);
    for (std::thread& t : pool) t.join();
    return dst;
}

// Complete every replacement's mip chain down to 1x1 with 2x2 box filtering,
// whatever levels the pack supplied: the sampler minifies a replacement as far
// as its original (samplerFor), and without the lower levels a far wall
// samples an 8192x8192 replacement's top level and shimmers as the camera
// moves (Noki Bay's undersea walls, whose pack files have one level).
// Compressed levels take a while to encode, so outside SMS_TEXTURE_PACK_SYNC
// they follow the pack's own levels instead of delaying them (completeLater).
static void completeChain(Loaded* L) {
    if (L->levels.empty()) return;
    int lw = L->w, lh = L->h;
    for (size_t i = 1; i < L->levels.size(); i++) {
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }
    if (L->compressed) {
        while (lw > 1 || lh > 1) {
            L->levels.push_back(nextBlockLevel(L->blocks, L->levels.back(), lw, lh));
            lw = std::max(1, lw / 2);
            lh = std::max(1, lh / 2);
        }
        return;
    }
    while (lw > 1 || lh > 1) {
        int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        const std::vector<uint8_t>& src = L->levels.back();
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                int x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
                int y0 = std::min(2 * y, lh - 1), y1 = std::min(2 * y + 1, lh - 1);
                for (int c = 0; c < 4; c++) {
                    int sum = src[(size_t(y0) * lw + x0) * 4 + c] + src[(size_t(y0) * lw + x1) * 4 + c] +
                              src[(size_t(y1) * lw + x0) * 4 + c] + src[(size_t(y1) * lw + x1) * 4 + c];
                    dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
                }
            }
        L->levels.push_back(std::move(dst));
        lw = nw;
        lh = nh;
    }
}

// `whole`: also the compressed levels completeChain makes, else only the
// pack's (RGBA images are always completed: they are decoded anyway).
static Loaded* decode(const PackFile& f, bool whole) {
    Loaded* L = new Loaded;
    L->arbitrary = f.arbitrary;
    if (endsWith(f.path, ".dds") || endsWith(f.path, ".DDS")) {
        if (!decodeDds(f.path, L)) {
            logmsg("texture pack: cannot read %s (not a DDS of a supported format)", f.path.c_str());
            L->failed = true;
        }
        if (!L->failed) {
            int lw = std::max(1, L->w >> (L->levels.size() - 1));
            int lh = std::max(1, L->h >> (L->levels.size() - 1));
            for (size_t level = L->levels.size(); level <= f.mips.size() && (lw > 1 || lh > 1); ++level) {
                const std::string& path = f.mips[level - 1];
                Loaded mip;
                lw = std::max(1, lw / 2);
                lh = std::max(1, lh / 2);
                if (path.empty() || !decodeDds(path, &mip) || mip.w != lw || mip.h != lh || mip.compressed != L->compressed)
                    break;
                L->levels.push_back(std::move(mip.levels.front()));
            }
        }
        if (whole || !L->compressed) completeChain(L);
        return L;
    }
    int n = 0;
    uint8_t* px = stbi_load(f.path.c_str(), &L->w, &L->h, &n, 4);
    if (!px) {
        logmsg("texture pack: cannot read %s (%s)", f.path.c_str(), stbi_failure_reason());
        L->failed = true;
        return L;
    }
    L->levels.emplace_back(px, px + size_t(L->w) * L->h * 4);
    stbi_image_free(px);
    int lw = L->w, lh = L->h;
    for (const std::string& mp : f.mips) {  // explicit levels, in order, while their sizes fit
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
        int mw, mh;
        uint8_t* m = mp.empty() ? nullptr : stbi_load(mp.c_str(), &mw, &mh, &n, 4);
        if (!m || mw != lw || mh != lh) {
            if (m) stbi_image_free(m);
            break;
        }
        L->levels.emplace_back(m, m + size_t(mw) * mh * 4);
        stbi_image_free(m);
    }
    completeChain(L);
    return L;
}

static void worker() {
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(s_mu);
            // The backlog can exceed its byte budget by one result, allowing
            // even a single texture larger than the budget to make progress.
            s_cv.wait(lk, [] { return s_stop || ((!s_queue.empty() || !s_restQueue.empty()) &&
                s_decoded.size() < 64 && s_decodedBytes < pendingBudget()); });
            if (s_stop) return;
            std::deque<Request>& q = s_queue.empty() ? s_restQueue : s_queue;
            req = std::move(q.front());
            q.pop_front();
            (req.rest ? s_busyRest : s_busy) = true;
        }
        std::unique_ptr<Loaded> L(decode(s_index.at(req.name), false));
        if (req.rest && !L->failed) {  // only the levels the pack left out
            L->first = int(L->levels.size());
            completeChain(L.get());
            L->levels.erase(L->levels.begin(), L->levels.begin() + L->first);
        }
        size_t bytes = 0;
        for (const auto& level : L->levels) bytes += level.size();
        std::lock_guard<std::mutex> lk(s_mu);
        (req.rest ? s_busyRest : s_busy) = false;
        if (s_stop) return;
        s_decodedBytes += bytes;
        s_decoded.push_back({std::move(req), std::move(L), bytes});
        s_cv.notify_all();
    }
}

static void startWorker() {
    if (!syncLoading() && !s_worker) {
        s_stop = false;
        s_worker = new std::thread(worker);
    }
}

// The worker makes the compressed levels a replacement's file leaves out
// after its own levels are in GL, behind every replacement still to read.
static void completeLater(const std::string& name) {
    std::lock_guard<std::mutex> lk(s_mu);
    s_restQueue.push_back({name, 0, 0, true});
    s_cv.notify_all();
}

static void uploadRest(Replacement& r, Loaded* L, int unit) {
    glcBindTexture(unit, r.tex);
    int lw = std::max(1, r.w >> L->first), lh = std::max(1, r.h >> L->first);
    int n = L->first;
    for (const auto& lv : L->levels) {
        glCompressedTexImage2D(GL_TEXTURE_2D, n++, L->compressed, lw, lh, 0, GLsizei(lv.size()), lv.data());
        r.bytes += lv.size();
        s_bytes += lv.size();
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1);
    r.levels = n;
}

static int chainLength(int w, int h) {
    int n = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) n++;
    return n;
}

static void upload(Replacement& r, Loaded* L, int unit, uint32_t gxW, uint32_t gxH, const std::string& name) {
    if (L->failed) {
        r.state = Replacement::FAILED;
        return;
    }
    int scale = 0;
    while (scale < 6 && (gxW << (scale + 1)) <= uint32_t(L->w) && (gxH << (scale + 1)) <= uint32_t(L->h)) scale++;
    glGenTextures(1, &r.tex);
    glcBindTexture(unit, r.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    int lw = L->w, lh = L->h, n = 0;
    r.bytes = 0;
    for (const auto& lv : L->levels) {
        r.bytes += lv.size();
        if (L->compressed)
            glCompressedTexImage2D(GL_TEXTURE_2D, n, L->compressed, lw, lh, 0, GLsizei(lv.size()), lv.data());
        else
            glTexImage2D(GL_TEXTURE_2D, n, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv.data());
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
        n++;
    }
    // Sampling stops at the last level until completeLater's arrive.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, int(L->levels.size()) - 1);
    r.w = L->w;
    r.h = L->h;
    r.levels = int(L->levels.size());
    if (r.levels < chainLength(L->w, L->h)) completeLater(name);
    r.scale = scale;
    r.state = Replacement::READY;
    s_bytes += r.bytes;
    s_uploaded++;
    extern uint32_t g_statTexUploads;
    g_statTexUploads++;
}

// The GL texture replacing `name` (a name hiresName found in the pack), or 0
// while it is still preparing or if it cannot be read. Normal draws only
// request/look up replacements; uploads happen at frame boundaries/loading.
// SMS_TEXTURE_PACK_SYNC=1 decodes on the spot (repeatable captures).
GLuint hiresTexture(const std::string& name, int unit, uint32_t gxW, uint32_t gxH, int* scale) {
    auto it = s_repl.find(name);
    if (it == s_repl.end()) {
        Replacement& r = s_repl[name];
        if (syncLoading()) {
            Loaded* L = decode(s_index.at(name), true);
            upload(r, L, unit, gxW, gxH, name);
            delete L;
        } else {
            requestTexture(name, gxW, gxH);
        }
        it = s_repl.find(name);
    }
    if (it->second.state != Replacement::READY) return 0;
    it->second.used = s_frame;
    *scale = it->second.scale;
    return it->second.tex;
}

static void uploadPending(bool wait) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    size_t bytes = 0;
    // Soft limits between textures: a single large upload cannot be interrupted.
    do {
        Decoded item;
        {
            std::unique_lock<std::mutex> lk(s_mu);
            if (wait) s_cv.wait(lk, [] { return !s_decoded.empty() || (s_queue.empty() && !s_busy); });
            if (s_decoded.empty()) break;
            item = std::move(s_decoded.front());
            s_decoded.pop_front();
            s_decodedBytes -= item.bytes;
            s_cv.notify_all();
        }
        bytes += item.bytes;
        if (item.request.rest) {  // dropped if its replacement was freed or reloaded meanwhile
            auto it = s_repl.find(item.request.name);
            Loaded* L = item.image.get();
            if (it != s_repl.end() && it->second.state == Replacement::READY && it->second.levels == L->first &&
                !L->failed && !L->levels.empty())
                uploadRest(it->second, L, 0);
            continue;
        }
        Replacement& r = s_repl[item.request.name];
        upload(r, item.image.get(), 0, item.request.w, item.request.h, item.request.name);
        r.used = s_frame;  // keep preloaded textures through the first draw
    } while (wait || (bytes < (16u << 20) && Clock::now() - start < std::chrono::milliseconds(2)));
}

void hiresPreload() {
    if (s_state != 1 || !preloading()) return;
    const auto start = std::chrono::steady_clock::now();
    uint32_t before = s_uploaded;
    uploadPending(true);
    if (s_uploaded != before)
        logmsg("texture pack: prepared %u replacements before gameplay in %.1f ms (%zu MiB resident)",
               s_uploaded - before,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), s_bytes >> 20);
}

// Once a display frame: over the memory budget (SMS_TEXTURE_PACK_MB, 1536
// by default), the replacements unused for longest are freed, down to three
// quarters of it. One sampled since the previous frame is never freed; a
// freed one is read again the next time its texture is.
void hiresEndFrame() {
    if (s_state == 1 && !syncLoading()) uploadPending(false);
    s_frame++;
    static size_t budget = 0;
    if (!budget) {
        const char* e = getenv("SMS_TEXTURE_PACK_MB");
        budget = size_t(e && atoi(e) > 0 ? atoi(e) : 1536) << 20;
    }
    if (s_bytes <= budget) return;
    std::vector<std::pair<uint32_t, const std::string*>> old;
    for (auto& kv : s_repl)
        if (kv.second.state == Replacement::READY && kv.second.used + 1 < s_frame) old.emplace_back(kv.second.used, &kv.first);
    std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t freed = 0, n = 0;
    std::vector<std::string> names;
    for (auto& o : old) {
        if (s_bytes - freed <= budget / 4 * 3) break;
        freed += s_repl[*o.second].bytes;
        names.push_back(*o.second);
    }
    for (const std::string& nm : names) {
        Replacement& r = s_repl[nm];
        glcForgetTexture(r.tex);
        glDeleteTextures(1, &r.tex);
        s_repl.erase(nm);
        std::lock_guard<std::mutex> lk(s_mu);
        s_requested.erase(nm);
        n++;
    }
    s_bytes -= freed;
    if (n) logmsg("texture pack: freed %zu replacements (%zu MiB) over the %zu MiB budget", n, freed >> 20, budget >> 20);
}

void hiresShutdown() {
    s_state = 0;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        s_stop = true;
        s_cv.notify_all();
    }
    if (s_worker) {
        s_worker->join();
        delete s_worker;
        s_worker = nullptr;
    }
    s_queue.clear();
    s_restQueue.clear();
    s_decoded.clear();
    s_requested.clear();
    s_decodedBytes = 0;
    s_busy = s_busyRest = false;
    s_bytes = 0;
    for (auto& kv : s_repl)
        if (kv.second.tex) {
            glcForgetTexture(kv.second.tex);
            glDeleteTextures(1, &kv.second.tex);
        }
    s_repl.clear();
    s_index.clear();
    s_s3tc = s_bptc = false;
    s_state = -1;
}

uint32_t hiresUploadedCount() { return s_uploaded; }

HiresStats hiresStats() {
    std::lock_guard<std::mutex> lk(s_mu);
    size_t rest = 0;
    for (const Decoded& d : s_decoded) rest += d.request.rest;
    return {s_bytes, s_decodedBytes, s_queue.size() + s_decoded.size() - rest + size_t(s_busy), s_uploaded,
            s_restQueue.size() + rest + size_t(s_busyRest)};
}

}  // namespace gx
