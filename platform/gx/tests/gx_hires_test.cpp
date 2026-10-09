// HD pack regressions: exercise actual GL uploads with generated DDS/resources.
#include "sms_gx/gx_pc.h"
#include "gx_internal.h"
#include "gl_funcs.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec.h"

void OSPanic(const char*, int, const char*, ...) { std::abort(); }
void DCFlushRange(void* p, uint32_t n) { GXPC_InvalidateRange(p, n); }
static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
static void env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}
static void le32(std::vector<uint8_t>& d, size_t p, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[p+i] = uint8_t(v >> (8*i));
}
static void be32(std::vector<uint8_t>& d, size_t p, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[p+i] = uint8_t(v >> (8*(3-i)));
}
static std::vector<uint8_t> bti(int id) {
    std::vector<uint8_t> d(0x40, uint8_t(id));
    std::fill(d.begin(), d.begin()+0x20, 0);
    d[0] = 1; d[3] = 8; d[5] = 4;
    be32(d, 0x1C, 0x20);
    return d;
}
static std::string textureName(const std::vector<uint8_t>& d) {
    return gx::hiresName(d.data()+0x20, 1, 8, 4, false, nullptr, 0, false);
}
static size_t dds(const std::filesystem::path& path, int w, int h, int mips, bool bc7 = false) {
    std::vector<uint8_t> d(bc7 ? 148 : 128);
    std::memcpy(d.data(), "DDS ", 4);
    le32(d, 4, 124); le32(d, 8, 0x21007); le32(d, 12, h); le32(d, 16, w);
    le32(d, 28, mips); le32(d, 76, 32); le32(d, 80, 4);
    std::memcpy(d.data()+84, bc7 ? "DX10" : "DXT1", 4);
    if (bc7) { le32(d, 128, 98); le32(d, 132, 3); le32(d, 140, 1); }
    const size_t header = d.size();
    for (int level = 0; level < mips; ++level) {
        size_t off = d.size(), n = size_t((w+3)/4)*((h+3)/4)*(bc7 ? 16 : 8);
        d.resize(off+n);
        for (size_t p = off; p < d.size(); p += bc7 ? 16 : 8) {
            if (bc7) d[p] = 0x40; // valid mode 6, transparent black
            else { d[p] = 0; d[p+1] = 0xF8; } // BC1 red endpoint, all indices 0
        }
        w = std::max(1, w/2); h = std::max(1, h/2);
    }
    std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(d.data()), d.size());
    return d.size()-header;
}
static PFNGLCOMPRESSEDTEXIMAGE2DPROC realCompressed;
static PFNGLTEXIMAGE2DPROC realImage;
static PFNGLTEXPARAMETERIPROC realParam;
static PFNGLGETINTEGERVPROC realGet;
static int compressedCalls, rgbaCalls, maxLevel;
static size_t uploadedBytes;
static void APIENTRY recordCompressed(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void* p) {
    ++compressedCalls; uploadedBytes += n;
    realCompressed(t,l,f,w,h,b,n,p);
}
static void APIENTRY recordImage(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void* p) {
    ++rgbaCalls; uploadedBytes += size_t(w)*h*4;
    realImage(t,l,f,w,h,b,fmt,ty,p);
}
static void APIENTRY recordParam(GLenum t, GLenum pname, GLint value) {
    if (pname == GL_TEXTURE_MAX_LEVEL) maxLevel = value;
    realParam(t,pname,value);
}
static void APIENTRY noCompression(GLenum name, GLint* value) {
    if (name == GL_NUM_EXTENSIONS) *value = 0;
    else if (name == GL_MAJOR_VERSION || name == GL_MINOR_VERSION) *value = 3;
    else realGet(name,value);
}
static void resetRecord() { compressedCalls = rgbaCalls = 0; uploadedBytes = 0; maxLevel = -1; }

// Encodes a block for the levels a pack leaves out and decodes it with bcdec;
// the largest difference of the checked channels (from `first`).
static int roundTrip(int format, const uint8_t* rgba, int channels, int first = 0) {
    uint8_t block[16], out[64];
    gx::encodeBlock(format, rgba, block);
    if (format == gx::BLOCK_BC1) bcdec_bc1(block, out, 16);
    else if (format == gx::BLOCK_BC2) bcdec_bc2(block, out, 16);
    else if (format == gx::BLOCK_BC3) bcdec_bc3(block, out, 16);
    else bcdec_bc7(block, out, 16);
    int worst = 0;
    for (int i = 0; i < 64; i++)
        if (i % 4 >= first && i % 4 < channels) worst = std::max(worst, std::abs(int(out[i]) - int(rgba[i])));
    return worst;
}

static void checkEncoder() {
    // ramp: 7 steps along the diagonal; steps: 4 steps a palette of 4 holds
    // (opaque: the BC1 copy); two: two colours; cut: BC1's transparent pixels.
    uint8_t ramp[64], steps[64], opaque[64], flat[64], two[64], cut[64];
    for (int i = 0; i < 16; i++) {
        int t = (i % 4) + (i / 4), u = i % 4;
        uint8_t r[4] = {uint8_t(40 + 30 * t), uint8_t(200 - 25 * t), uint8_t(90 + 5 * t), uint8_t(255 - 40 * t)};
        std::memcpy(ramp + 4 * i, r, 4);
        uint8_t s[4] = {uint8_t(33 + 66 * u), uint8_t(231 - 66 * u), uint8_t(66 + 33 * u), uint8_t(255 - 66 * u)};
        std::memcpy(steps + 4 * i, s, 4);
        s[3] = 255;
        std::memcpy(opaque + 4 * i, s, 4);
        uint8_t f[4] = {201, 99, 47, 255};
        std::memcpy(flat + 4 * i, f, 4);
        uint8_t c[4] = {uint8_t(i & 1 ? 230 : 20), 120, uint8_t(i & 1 ? 20 : 230), uint8_t(i & 1 ? 40 : 250)};
        std::memcpy(two + 4 * i, c, 4);
        c[3] = uint8_t(i & 2 ? 255 : 0);
        std::memcpy(cut + 4 * i, c, 4);
    }
    expect(roundTrip(gx::BLOCK_BC7, ramp, 4) <= 8, "BC7 encodes a colour and alpha ramp");
    expect(roundTrip(gx::BLOCK_BC7, flat, 4) <= 1, "BC7 encodes a flat colour");
    expect(roundTrip(gx::BLOCK_BC7, two, 4) <= 2, "BC7 encodes two colours");
    // A glow's 8x8 level: white where it is clear, alpha peaking on a grey core.
    static const uint8_t glow[64] = {
        255, 255, 255, 1,  252, 252, 252, 1,  159, 159, 159, 1,  102, 102, 102, 13,
        252, 252, 252, 1,  81,  81,  81,  11, 24,  24,  24,  38, 24,  24,  24,  62,
        159, 159, 159, 1,  24,  24,  24,  38, 30,  30,  30,  80, 92,  92,  92,  126,
        102, 102, 102, 13, 24,  24,  24,  62, 92,  92,  92,  126, 213, 213, 213, 214};
    expect(roundTrip(gx::BLOCK_BC7, glow, 4, 3) <= 40, "BC7 keeps alpha that does not follow colour");
    expect(roundTrip(gx::BLOCK_BC1, opaque, 3) <= 8, "BC1 encodes four steps of colour");
    expect(roundTrip(gx::BLOCK_BC1, flat, 3) <= 4, "BC1 encodes a flat colour");
    expect(roundTrip(gx::BLOCK_BC2, steps, 4) <= 9, "BC2 encodes four steps of colour and alpha");
    expect(roundTrip(gx::BLOCK_BC3, steps, 4) <= 14, "BC3 encodes four steps of colour and alpha");
    expect(roundTrip(gx::BLOCK_BC3, ramp, 4) <= 32, "BC3 encodes a colour and alpha ramp");
    uint8_t block[8], out[64];
    gx::encodeBlock(gx::BLOCK_BC1, cut, block);
    bcdec_bc1(block, out, 16);
    bool punch = true;
    for (int i = 0; i < 16; i++) punch &= out[4 * i + 3] == (i & 2 ? 255 : 0);
    expect(punch, "BC1 keeps transparent pixels transparent");
}

int main(int argc, char** argv) {
    const bool fallback = argc > 1 && std::strcmp(argv[1], "--fallback") == 0;
    const bool sync = argc > 1 && std::strcmp(argv[1], "--sync") == 0;
    const bool disabled = argc > 1 && std::strcmp(argv[1], "--no-preload") == 0;
    checkEncoder();
    env("SMS_TEXTURE_PACK_SYNC", sync ? "1" : "0");
    env("SMS_TEXTURE_PACK_PRELOAD", disabled ? "0" : "1");
    auto dir = std::filesystem::temp_directory_path() /
        ("sms-hires-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto single = bti(1), partial = bti(2), bptc = bti(3), missing = bti(4), late = bti(5), external = bti(6);
    dds(dir/(textureName(single)+".dds"), 16, 8, 1);
    dds(dir/(textureName(partial)+".dds"), 32, 16, 3);
    dds(dir/(textureName(bptc)+".dds"), 16, 8, 1, true);
    { std::ofstream f(dir/(textureName(missing)+".dds")); f << "not a DDS"; }
    dds(dir/(textureName(late)+".dds"), 16, 8, 1);
    dds(dir/(textureName(external)+".dds"), 16, 8, 1);
    dds(dir/(textureName(external)+"_mip1.dds"), 8, 4, 1);
    std::vector<std::vector<uint8_t>> many;
    for (int i = 10; i < 34; ++i) {
        many.push_back(bti(i));
        dds(dir/(textureName(many.back())+".dds"), 512, 512, 1);
    }
    env("SMS_TEXTURE_PACKS", "0"); env("SMS_TEXTURE_PACK_PENDING_MB", "1"); env("SMS_TEXTURE_PACK_MB", "1");
    GXPC_SetHeadless(1); GXPC_SetAutoPresent(0);
    if (!GXPC_InitAuto(1)) { std::filesystem::remove_all(dir); return 77; }
    gx::hiresShutdown();
    env("SMS_TEXTURE_PACKS", dir.string());
    realGet = gx::gl::gx_glGetIntegerv;
    if (fallback) gx::gl::gx_glGetIntegerv = noCompression;
    expect(gx::hiresEnabled(), "texture index starts after context initialization");
    gx::gl::gx_glGetIntegerv = realGet;
    realCompressed = gx::gl::gx_glCompressedTexImage2D; realImage = gx::gl::gx_glTexImage2D;
    realParam = gx::gl::gx_glTexParameteri;
    gx::gl::gx_glCompressedTexImage2D = recordCompressed; gx::gl::gx_glTexImage2D = recordImage;
    gx::gl::gx_glTexParameteri = recordParam;
    auto prepare = [&](const std::vector<uint8_t>& resource, const char* name) {
        resetRecord();
        // A loader thread can disappear and free its resource before the upload.
        std::thread loader([&] { auto copy = resource; GXPC_PrefetchResource(copy.data(), copy.size(), name); });
        loader.join();
        GXPC_PreloadTextures();
    };
    // The compressed levels a file leaves out are made after the load and
    // uploaded with later work (here without a frame end's eviction).
    auto complete = [&] {
        for (int i = 0; i < 2000 && gx::hiresStats().completing; ++i) {
            GXPC_PreloadTextures(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    prepare(single, "single.bti");
    if (sync || disabled) {
        expect(uploadedBytes == 0 && gx::hiresStats().pendingCount == 0, "resource preload obeys sync/disabled setting");
        int scale = 0;
        auto tex = gx::hiresTexture(textureName(single), 0, 8, 4, &scale);
        if (sync) {
            expect(tex != 0 && compressedCalls == 5, "sync setting prepares a replacement immediately on first use");
        } else {
            expect(tex == 0 && uploadedBytes == 0, "disabled preload keeps draw-time requests asynchronous");
            for (int i = 0; i < 500 && !tex; ++i) {
                gx::hiresEndFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
                tex = gx::hiresTexture(textureName(single), 0, 8, 4, &scale);
            }
            expect(tex != 0, "disabled preload still streams replacement at frame boundaries");
        }
        GXPC_Shutdown(); std::filesystem::remove_all(dir);
        return failures ? 1 : 0;
    }
    expect(fallback ? (rgbaCalls == 5 && uploadedBytes == 684) :
        (compressedCalls == 1 && rgbaCalls == 0 && uploadedBytes == 64), "single-level DDS retains blocks; unsupported GPU gets complete RGBA mips");
    expect(maxLevel == (fallback ? 4 : 0), "single-level texture samples its own level until the rest are made");
    complete();
    expect(fallback ? rgbaCalls == 5 : (compressedCalls == 5 && uploadedBytes == 104 && maxLevel == 4),
        "single-level DDS gains its other levels, encoded as blocks, after the load");
    int scale = 0;
    expect(gx::hiresTexture(textureName(single), 0, 8, 4, &scale) != 0 && scale == 1, "preloaded texture is ready on first use with correct LOD scale");
    // A J3D TEX1 table with a header at 0x20 and its relative image offset.
    std::vector<uint8_t> model(0x80);
    std::memcpy(model.data(), "J3D2bmd3", 8); be32(model, 0x0C, 1);
    std::memcpy(model.data()+0x20, "TEX1", 4); be32(model, 0x24, 0x60);
    model[0x29] = 1; be32(model, 0x2C, 0x20);
    std::copy(partial.begin(), partial.end(), model.begin()+0x40);
    prepare(model, "map.bmd");
    expect(fallback ? rgbaCalls == 6 : (compressedCalls == 3 && uploadedBytes == 336), "offscreen J3D textures preload all supplied DDS levels");
    expect(maxLevel == (fallback ? 5 : 2), "partial DDS samples its supplied levels until the rest are made");
    complete();
    expect(fallback ? rgbaCalls == 6 : (compressedCalls == 6 && uploadedBytes == 360 && maxLevel == 5),
        "partial DDS is completed down to 1x1");
    std::vector<uint8_t> particle(0x80);
    std::memcpy(particle.data(), "JEFFjpa1", 8); be32(particle, 0x0C, 1);
    std::memcpy(particle.data()+0x20, "TEX1", 4); be32(particle, 0x24, 0x60);
    std::copy(bptc.begin(), bptc.end(), particle.begin()+0x40);
    prepare(particle, "effect.jpa");
    expect(fallback ? rgbaCalls == 5 : (compressedCalls == 1 && uploadedBytes == 128), "particle BC7 texture preloads without expanding on a supported GPU");
    complete();
    expect(fallback ? rgbaCalls == 5 : (compressedCalls == 5 && uploadedBytes == 208 && maxLevel == 4),
        "BC7 texture gains its other levels as BC7");
    prepare(external, "external.bti");
    expect(fallback ? rgbaCalls == 5 : (compressedCalls == 2 && uploadedBytes == 80 && maxLevel == 1), "DDS external mip files are preserved with compatible dimensions and format");
    complete();
    expect(fallback ? rgbaCalls == 5 : (compressedCalls == 5 && uploadedBytes == 104 && maxLevel == 4),
        "external mip files are completed after their last level");
    expect(glGetError() == GL_NO_ERROR, "compressed/partial/fallback uploads are accepted by GL");
    resetRecord();
    GXPC_PrefetchResource(model.data(), 0x2C, "truncated.bmd");
    be32(model, 0x2C, 0xFFFFFFF0);
    GXPC_PrefetchResource(model.data(), model.size(), "bad-offset.bmd");
    GXPC_PreloadTextures();
    expect(uploadedBytes == 0, "malformed resource offsets and lengths are ignored safely");
    prepare(missing, "missing.bti");
    expect(gx::hiresTexture(textureName(missing), 0, 8, 4, &scale) == 0, "bad DDS leaves original texture available and does not stall preload");
    resetRecord();
    expect(gx::hiresTexture(textureName(late), 0, 8, 4, &scale) == 0 && uploadedBytes == 0, "late texture lookup never uploads inside a draw");
    for (int i = 0; i < 500 && !gx::hiresTexture(textureName(late), 0, 8, 4, &scale); ++i) {
        gx::hiresEndFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(uploadedBytes > 0 && gx::hiresTexture(textureName(late), 0, 8, 4, &scale) != 0, "late texture becomes ready through frame-boundary uploads");
    for (const auto& resource : many) GXPC_PrefetchResource(resource.data(), resource.size(), "many.bti");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(gx::hiresStats().decodedBytes <= (1u<<20) + (fallback ? 1398100 : 131072), "decoder backlog stays within budget plus one in-flight texture");
    GXPC_PreloadTextures();
    expect(gx::hiresStats().pendingCount == 0, "preload drains a backpressured worker without deadlocking");
    complete();
    resetRecord();
    for (const auto& resource : many) GXPC_PrefetchResource(resource.data(), resource.size(), "again.bti");
    GXPC_PreloadTextures();
    expect(uploadedBytes == 0, "duplicate resource fetches reuse prepared replacements");
    gx::hiresTexture(textureName(single), 0, 8, 4, &scale);
    gx::hiresEndFrame();
    gx::hiresTexture(textureName(single), 0, 8, 4, &scale);
    gx::hiresEndFrame();
    expect(gx::hiresStats().residentBytes <= (1u<<20), "GPU cache frees old replacements when over budget");
    expect(gx::hiresTexture(textureName(single), 0, 8, 4, &scale) != 0, "eviction protects a texture sampled in the previous frame");
    bool reloaded = false;
    for (const auto& resource : many) {
        if (!gx::hiresTexture(textureName(resource), 0, 8, 4, &scale)) {
            prepare(resource, "reload.bti");
            reloaded = uploadedBytes > 0;
            break;
        }
    }
    expect(reloaded, "evicted replacements can be requested and prepared again");
    // Shut down with more work queued, then rebuild the index in the same context.
    gx::hiresShutdown();
    gx::hiresEnabled();
    for (const auto& resource : many) GXPC_PrefetchResource(resource.data(), resource.size(), "shutdown.bti");
    gx::hiresShutdown();
    expect(gx::hiresStats().pendingCount == 0 && gx::hiresStats().decodedBytes == 0, "shutdown joins worker and releases queued/decoded resources");
    GXPC_Shutdown();
    std::filesystem::remove_all(dir);
    std::printf("HD texture tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
