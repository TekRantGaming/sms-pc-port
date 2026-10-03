// Verify HD tile pixels independently of the native GX texture size limit.
#include <sms_hd_movies.h>
#include <THPPlayer/THPDraw.h>
#include <dolphin/os.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int full_width, full_height, draw_width, draw_height, calls;
struct Draw { u8* planes[3]; int left, top, width, height; };
std::vector<Draw> draws;
u8 sample(int x, int y, int plane) { return (x * 3 + y * 7 + plane * 47) % 251; }
size_t address(int width, int x, int y)
{
    return ((y / 4) * (width / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8;
}
void check_pixels()
{
    // Check after every tile was submitted, so overwriting an earlier tile's
    // storage with a later one is detected as well as incorrect crop strides.
    for (const Draw& d : draws)
        for (int p = 0; p < 3; ++p) {
            const int divisor = p ? 2 : 1;
            assert(!(reinterpret_cast<uintptr_t>(d.planes[p]) & 31));
            for (int y = 0; y < d.height / divisor; ++y)
                for (int x = 0; x < d.width / divisor; ++x)
                    assert(d.planes[p][address(d.width / divisor, x, y)]
                           == sample(x + d.left / divisor, y + d.top / divisor, p));
        }
}
}
extern "C" void* port_low_alloc(unsigned long size) { return std::malloc(size); }
extern "C" void port_log(const char*, ...) {}
extern "C" void DCFlushRange(void*, u32) {}
extern "C" void THPGXYuv2RgbDraw(u8* y, u8* u, u8* v, s16 x, s16 top,
                                 s16 width, s16 height, s16 pw, s16 ph)
{
    const int columns = (full_width + 959) / 960;
    const int sx = (calls % columns) * 960, sy = (calls / columns) * 960;
    assert(width <= 960 && height <= 960);
    assert(x == 17 + sx * draw_width / full_width);
    assert(top == 29 + sy * draw_height / full_height);
    assert(x + pw == 17 + (sx + width) * draw_width / full_width);
    assert(top + ph == 29 + (sy + height) * draw_height / full_height);
    draws.push_back({{y, u, v}, sx, sy, width, height});
    ++calls;
}
int main()
{
    for (const auto& size : {std::pair<int, int>(1920, 960), {1920, 1344},
                             {1280, 896}, {2048, 2048}, {640, 448}}) {
        full_width = size.first; full_height = size.second;
        draw_width = 640; draw_height = 448; calls = 0; draws.clear();
        std::vector<u8> planes[3];
        for (int p = 0; p < 3; ++p) {
            const int width = full_width / (p ? 2 : 1), height = full_height / (p ? 2 : 1);
            planes[p].resize(width * height);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    planes[p][address(width, x, y)] = sample(x, y, p);
        }
        const bool hd = full_width > 1024 || full_height > 1024;
        assert(bool(port_draw_hd_thp(planes[0].data(), planes[1].data(), planes[2].data(),
                                    17, 29, full_width, full_height, draw_width, draw_height)) == hd);
        assert(calls == (hd ? ((full_width + 959) / 960) * ((full_height + 959) / 960) : 0));
        check_pixels();
        std::printf("PASS %dx%d: %d tiles, all plane pixels and display edges\n", full_width, full_height, calls);
    }
}
