// HD movies use the original YUV renderer, split into GX-sized textures.
#include <sms_hd_movies.h>
#include <THPPlayer/THPDraw.h>
#include <dolphin/os.h>
#include "port_platform.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
// GX I8 is tiled in 8x4 blocks. Every source/crop boundary is aligned to
// 16 pixels, so copying complete tile rows also works for the chroma planes.
void copy_plane(u8* out, const u8* source, int source_width,
                int x, int y, int width, int height)
{
    for (int row = 0; row < height / 4; ++row)
        std::memcpy(out + row * width * 4,
                    source + (y / 4 + row) * source_width * 4 + x * 4,
                    width * 4);
    DCFlushRange(out, static_cast<u32>(width * height));
}
}

extern "C" BOOL port_draw_hd_thp(u8* y, u8* u, u8* v, s16 x, s16 pos_y,
                                s16 width, s16 height, s16 draw_width,
                                s16 draw_height)
{
    if (width <= 1024 && height <= 1024)
        return FALSE;
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048
        || (width & 15) || (height & 15))
        return TRUE;

    // GX texture objects hold 32-bit addresses even on 64-bit hosts. Reserve
    // a low, aligned pool lazily, outside the game's MEM1 heap. Each tile gets
    // distinct storage until THPPlayerDrawDone flushes the complete frame.
    static u8* storage = nullptr;
    if (!storage) {
        void* pool = port_low_alloc(2048 * 2048 * 3 / 2 + 31);
        if (!pool) {
            port_log("[thp] Cannot allocate the HD movie texture pool\n");
            std::abort();
        }
        storage = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(pool) + 31)
                                        & ~static_cast<uintptr_t>(31));
    }
    constexpr int edge = 960;
    u8* cursor = storage;
    for (int top = 0; top < height; top += edge) {
        const int h = std::min(edge, height - top);
        for (int left = 0; left < width; left += edge) {
            const int w = std::min(edge, width - left);
            u8* ty = cursor;
            u8* tu = ty + w * h;
            u8* tv = tu + w * h / 4;
            cursor = tv + w * h / 4;
            copy_plane(ty, y, width, left, top, w, h);
            copy_plane(tu, u, width / 2, left / 2, top / 2, w / 2, h / 2);
            copy_plane(tv, v, width / 2, left / 2, top / 2, w / 2, h / 2);
            const int x0 = x + left * draw_width / width;
            const int y0 = pos_y + top * draw_height / height;
            const int x1 = x + (left + w) * draw_width / width;
            const int y1 = pos_y + (top + h) * draw_height / height;
            // Recursion stops here: these textures fit the native GX limit.
            THPGXYuv2RgbDraw(ty, tu, tv, x0, y0,
                            w, h, x1 - x0, y1 - y0);
        }
    }
    return TRUE;
}
