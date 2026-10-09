// Window placement regression checks; no game disc or save files required.
#include "gx_window_layout.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SMS_GX_HAVE_SDL2
#include <SDL.h>
#include "sms_gx/gx_pc.h"
void OSPanic(const char* file, int line, const char* msg, ...) {
    std::fprintf(stderr, "OSPanic %s:%d %s\n", file, line, msg);
    std::abort();
}
void DCFlushRange(void* p, uint32_t n) { GXPC_InvalidateRange(p, n); }
#endif

static int failures = 0;
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}

static void checkLayout(gx::WindowArea desktop, double aspect, int scale, gx::WindowBorders borders) {
    const auto window = gx::initialWindowLayout(desktop, aspect, scale, borders);
    const int outerW = window.w + borders.left + borders.right;
    const int outerH = window.h + borders.top + borders.bottom;
    expect(window.w > 0 && window.h > 0, "positive client size");
    expect(outerW <= int(desktop.w * 0.8) && outerH <= int(desktop.h * 0.8), "frame fits usable desktop");
    expect(window.x - borders.left >= desktop.x && window.y - borders.top >= desktop.y, "title bar stays on screen");
    expect(window.x + window.w + borders.right <= desktop.x + desktop.w &&
           window.y + window.h + borders.bottom <= desktop.y + desktop.h, "resize edges stay on screen");
    expect(std::abs(2 * (window.x - borders.left - desktop.x) + outerW - desktop.w) <= 1 &&
           std::abs(2 * (window.y - borders.top - desktop.y) + outerH - desktop.h) <= 1, "frame is centered");
    expect(std::abs(window.w - window.h * aspect) <= aspect + 1, "starting aspect ratio preserved");
}

int main(int argc, char** argv) {
    const gx::WindowArea desktops[] = {{0, 40, 1920, 1000}, {0, 0, 1366, 728}, {0, 24, 1280, 656},
        {0, 0, 640, 480}, {0, 0, 320, 240}, {-1920, 120, 1920, 1040}, {1920, -1080, 1080, 1920},
        {0, 24, 1512, 920}, {0, 0, 3840, 2120}};
    for (auto desktop : desktops)
        for (double aspect : {4.0 / 3, 16.0 / 9, 16.0 / 10, 21.0 / 9})
            for (int scale : {0, 1, 2, 4, 16})
                for (auto borders : {gx::WindowBorders{48, 8, 8, 8}, gx::WindowBorders{30, 0, 0, 0}})
                    checkLayout(desktop, aspect, scale, borders);
    const auto normal = gx::initialWindowLayout({0, 0, 1920, 1080}, 16.0 / 9);
    expect(normal.w == 1280 && normal.h == 720, "predictable default size on a 1080p screen");
    expect(gx::initialWindowLayout({0, 0, 3840, 2160}, 21.0 / 9).w == 1280, "ultrawide default remains manageable");
    const auto invalid = gx::initialWindowLayout({0, 0, 640, 480}, 0);
    expect(invalid.w > 0 && invalid.h > 0, "invalid aspect ratio has a safe fallback");

#ifdef SMS_GX_HAVE_SDL2
    if (argc > 1 && std::strcmp(argv[1], "--headless") == 0) {
        GXPC_SetHeadless(1);
        expect(GXPC_InitAuto(1) && GXPC_IsHeadless(), "headless rendering still initializes with fullscreen requested");
        expect(!SDL_GL_GetCurrentWindow(), "fullscreen does not create a window in headless mode");
        GXPC_Shutdown();
    }
    const bool fullscreen = argc > 1 && std::strcmp(argv[1], "--fullscreen") == 0;
    if (fullscreen || (argc > 1 && std::strcmp(argv[1], "--window") == 0)) {
        // Exercise the game's actual SDL/OpenGL bring-up, even at 4x quality.
        GXPC_SetHeadless(0);
        GXPC_SetWidescreen(4.0f / 3.0f);
        expect(GXPC_InitAuto(4) && !GXPC_IsHeadless(), "real game window opens");
        // the game's window (its GL context may be current on the GL thread)
        SDL_Window* window = nullptr;
        for (Uint32 id = 1; id < 64 && !window; id++) window = SDL_GetWindowFromID(id);
        expect(window != nullptr, "the game window can be found");
        if (!window) return 1;
        SDL_PumpEvents();
        int w, h, x, y, minW, minH;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowMinimumSize(window, &minW, &minH);
        std::printf("Initial game window: %dx%d at %d,%d\n", w, h, x, y);
        // Fullscreen transitions are asynchronous on some window managers.
        // Pump resize events until SDL reports the completed transition.
        const auto waitForSize = [&](auto ready) {
            for (int attempt = 0; attempt < 200; ++attempt) {
                SDL_PumpEvents();
                SDL_GetWindowSize(window, &w, &h);
                if (ready(w, h)) return true;
                SDL_Delay(10);
            }
            return false;
        };
        SDL_Rect desktop;
        expect(SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &desktop) == 0, "usable desktop available");
        const Uint32 flags = SDL_GetWindowFlags(window);
        if (fullscreen) {
            expect((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP,
                   "launcher fullscreen setting opens desktop fullscreen");
            SDL_Rect display = {};
            expect(SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(window), &display) == 0, "full display bounds available");
            expect(waitForSize([&](int width, int height) { return width == display.w && height == display.h; }),
                   "fullscreen fills the display at high render quality");
            SDL_GetWindowPosition(window, &x, &y);
            expect(x == display.x && y == display.y, "fullscreen starts at the display origin");
            std::printf("Fullscreen smoke: %dx%d at %d,%d\n", w, h, x, y);
            expect(SDL_SetWindowFullscreen(window, 0) == 0, "fullscreen can return to a window");
            expect(waitForSize([&](int width, int height) { return width <= desktop.w * 0.8 && height <= desktop.h * 0.8; }),
                   "leaving fullscreen restores the fitted window size");
        } else {
            expect(!(flags & SDL_WINDOW_FULLSCREEN), "fullscreen is off when not requested");
        }
        expect(w <= desktop.w * 0.8 && h <= desktop.h * 0.8, "window fits screen at high render quality");
        if (!fullscreen) {
            expect(x >= desktop.x && y >= desktop.y && x + w <= desktop.x + desktop.w && y + h <= desktop.y + desktop.h,
                   "real window fully visible");
            expect(std::abs(2 * x + w - 2 * desktop.x - desktop.w) <= 64 &&
                   std::abs(2 * y + h - 2 * desktop.y - desktop.h) <= 64, "real window is centered with decoration allowance");
        }
        expect(SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE, "native resizing is enabled");
        expect(minW <= 320 && minH <= 240, "minimum size allows shrinking");
        SDL_SetWindowSize(window, std::min(480, w), std::min(360, h));
        SDL_PumpEvents();
        SDL_GetWindowSize(window, &w, &h);
        expect(w <= 480 && h <= 360, "real window can be resized smaller");
        std::printf("Window smoke: desktop %dx%d at %d,%d; resized to %dx%d\n", desktop.w, desktop.h, desktop.x, desktop.y, w, h);
        GXPC_Shutdown();
        SDL_Quit();
    }
#else
    (void)argc; (void)argv;
#endif
    std::printf("Window placement: 360 display/aspect/scale/border scenarios, %d failures\n", failures);
    return failures ? 1 : 0;
}
