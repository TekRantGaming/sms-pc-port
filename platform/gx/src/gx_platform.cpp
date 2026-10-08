// Window / context bring-up for sms_gx.
//
// Default: an SDL2 window with an OpenGL 3.3 core context whenever a display
// is available.  Headless (tests, CI, no display): an EGL context without a
// surface.  GXInit brings this up automatically if the host has not called
// GXPC_Init itself, and every GXCopyDisp presents the XFB to the window.
#include "gx_internal.h"
#include "gx_window_layout.h"
#include "sms_gx/gx_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef SMS_GX_HAVE_SDL2
#include <SDL.h>
#endif
#ifdef SMS_GX_HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

namespace gx {
extern void (*g_displayCopyHook)(const void* xfb);
bool rendererReady();
extern double g_presentSeconds, g_swapSeconds;
}

using namespace gx;

namespace {
enum Mode { MODE_NONE, MODE_WINDOW, MODE_HEADLESS, MODE_EXTERNAL };
Mode s_mode = MODE_NONE;
int s_forceHeadless = -1;  // -1: decide from the environment
bool s_autoPresent = true;
bool s_skipPresent = false;
// Average seconds a present takes (drawing the XFB, the overlay and the swap).
// A skip request (GXPC_SkipNextPresent) is only followed when presenting is
// that slow: then the swap waits for the display and skipping one catches the
// game up; a quick present gains nothing by being left out.
double s_presentCost = 0;
const double kSkipPresentCost = 0.002;
double s_lastPresent = 0;  // the last display copy's present, 0 when skipped
int s_vsync = 0;
uint32_t s_frame = 0;

#ifdef SMS_GX_HAVE_SDL2
SDL_Window* s_window = nullptr;
SDL_GLContext s_glctx = nullptr;
void (*s_eventCb)(const SDL_Event*) = nullptr;
std::vector<SDL_GameController*> s_pads;
#endif
std::vector<uint8_t> s_icon;  // GXPC_SetWindowIcon, RGBA8
int s_iconW = 0, s_iconH = 0;

double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

bool envTrue(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

#ifdef SMS_GX_HAVE_SDL2
void* sdlGetProc(const char* name) { return SDL_GL_GetProcAddress(name); }

// SMS_PRESENT_HZ=<rate> (testing): each swap also waits for the next refresh
// of a display at that rate, as a compositor that forces vsync does (macOS,
// the Steam Deck's gamescope), whatever the vsync setting.
void waitSimulatedRefresh() {
    static double period = -1;
    if (period < 0) {
        const char* e = getenv("SMS_PRESENT_HZ");
        period = e && atof(e) > 0 ? 1.0 / atof(e) : 0;
    }
    if (period == 0) return;
    const double next = (double(int64_t(nowSeconds() / period)) + 1) * period;
    for (double now; (now = nowSeconds()) < next;)
        if (next - now > 0.002) SDL_Delay(Uint32((next - now) * 1000) - 1);
}

// SMS_MOUSE_CAMERA=1: mouse look. The mouse is captured (relative mode) while
// the window has focus; F10 releases it, a click in the window takes it back,
// and losing focus always frees it.
bool s_mouseCamera = false, s_mouseCaptured = false, s_mouseReleased = false;

void captureMouse(bool on) {
    if (!s_mouseCamera) on = false;
    if (on == s_mouseCaptured) return;
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0) s_mouseCaptured = on;
}

// SMS_FULLSCREEN: 1 or desktop (borderless, at the desktop's resolution), or
// exclusive (the display switches to SMS_FULLSCREEN_MODE=WxH[@Hz], else the
// desktop's mode). F11 or Alt+Enter toggles between the window and that
// fullscreen (desktop when SMS_FULLSCREEN is unset).
bool s_exclusive = false, s_isFullscreen = false;

void setFullscreen(bool on) {
    if (!s_window) return;
    Uint32 flags = 0;
    if (on && s_exclusive) {
        SDL_DisplayMode want = {}, got = {};
        const int display = std::max(0, SDL_GetWindowDisplayIndex(s_window));
        SDL_GetDesktopDisplayMode(display, &want);
        if (const char* e = getenv("SMS_FULLSCREEN_MODE")) {
            int w = 0, h = 0, hz = 0;
            if (sscanf(e, "%dx%d@%d", &w, &h, &hz) >= 2 && w > 0 && h > 0) {
                want.w = w;
                want.h = h;
                if (hz > 0) want.refresh_rate = hz;
            }
        }
        if (SDL_GetClosestDisplayMode(display, &want, &got)) SDL_SetWindowDisplayMode(s_window, &got);
        flags = SDL_WINDOW_FULLSCREEN;
        logmsg("exclusive fullscreen %dx%d@%dHz", got.w, got.h, got.refresh_rate);
    } else if (on) {
        flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    if (SDL_SetWindowFullscreen(s_window, flags) != 0) {
        logmsg("fullscreen change failed: %s; continuing as before", SDL_GetError());
        return;
    }
    s_isFullscreen = on;
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}

void applyIcon() {
    if (!s_window || s_icon.empty()) return;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(s_icon.data(), s_iconW, s_iconH, 32, s_iconW * 4,
                                                        SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    SDL_SetWindowIcon(s_window, s);
    SDL_FreeSurface(s);
}

// Why the last openWindow failed: no display to open a window on (SDL's video
// init failed), or a display whose window or OpenGL context could not be made.
enum WindowFailure { WF_NONE, WF_NO_DISPLAY, WF_WINDOW };
WindowFailure s_windowFailure = WF_NONE;
char s_videoDriver[32];     // the SDL video driver of the last attempt ("x11", "wayland", ...)
char s_windowError[256];    // and its SDL error

bool openWindow(int scale) {
#ifdef SDL_HINT_WINDOWS_DPI_SCALING
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
    s_windowFailure = WF_NONE;
    s_videoDriver[0] = 0;
    // Native GameCube adapters (Nintendo / Mayflash in Wii U mode, 057e:0337):
    // SDL's HIDAPI driver turns each plugged port into a mapped game controller.
    // Set before SDL_Init; an SDL_JOYSTICK_HIDAPI_GAMECUBE environment value wins.
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_GAMECUBE", "1");
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        logmsg("SDL_Init failed: %s", SDL_GetError());
        snprintf(s_windowError, sizeof s_windowError, "%s", SDL_GetError());
        s_windowFailure = WF_NO_DISPLAY;
        return false;
    }
    if (const char* d = SDL_GetCurrentVideoDriver()) snprintf(s_videoDriver, sizeof s_videoDriver, "%s", d);
    // from here on a display exists: a failure is the window's or the context's
    s_windowFailure = WF_WINDOW;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // Open on the monitor containing the pointer (usually the launcher's Play
    // button). Fall back to the primary display if global coordinates are unavailable.
    SDL_Point pointer = {};
    SDL_GetGlobalMouseState(&pointer.x, &pointer.y);
    int display = 0;
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) == 0 && SDL_PointInRect(&pointer, &bounds)) {
            display = i;
            break;
        }
    }
    // SMS_DISPLAY=n picks the monitor instead (0 is the primary one)
    if (const char* e = getenv("SMS_DISPLAY")) {
        const int want = atoi(e);
        if (*e && want >= 0 && want < SDL_GetNumVideoDisplays()) display = want;
    }
    SDL_Rect desktop = {0, 0, 1280, 800};
    if (SDL_GetDisplayUsableBounds(display, &desktop) != 0 &&
        SDL_GetDisplayBounds(display, &desktop) != 0) desktop = {0, 0, 1280, 800};
    int windowScale = 0;
    if (const char* e = getenv("SMS_WINDOW_SCALE")) {
        const long requested = strtol(e, nullptr, 10);
        if (requested > 0) windowScale = int(std::min(requested, 16L));
    }
    const WindowArea layout = initialWindowLayout({desktop.x, desktop.y, desktop.w, desktop.h},
                                                  640.0 * GXPC_GetWidescreen() / 480.0, windowScale);
    s_window = SDL_CreateWindow("Super Mario Sunshine", layout.x, layout.y, layout.w, layout.h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                SDL_WINDOW_HIDDEN);
    if (!s_window) {
        logmsg("SDL_CreateWindow failed (%s): %s", s_videoDriver, SDL_GetError());
        snprintf(s_windowError, sizeof s_windowError, "%s", SDL_GetError());
        SDL_Quit();
        return false;
    }
    SDL_SetWindowMinimumSize(s_window, std::min(320, layout.w), std::min(240, layout.h));
    applyIcon();
    s_glctx = SDL_GL_CreateContext(s_window);
    if (!s_glctx) {
        logmsg("OpenGL 3.3 core context failed (%s): %s", s_videoDriver, SDL_GetError());
        snprintf(s_windowError, sizeof s_windowError, "OpenGL 3.3: %s", SDL_GetError());
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(s_window, s_glctx);
    // adaptive vsync (-1) tears only when a frame is late; not every driver has it
    if (SDL_GL_SetSwapInterval(s_vsync) != 0 && s_vsync < 0) SDL_GL_SetSwapInterval(1);
    if (!GXPC_Init(sdlGetProc, scale)) {
        snprintf(s_windowError, sizeof s_windowError, "the renderer could not start on this OpenGL context");
        SDL_GL_DeleteContext(s_glctx);
        SDL_DestroyWindow(s_window);
        s_glctx = nullptr;
        s_window = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_ShowWindow(s_window);
    // Some window managers choose their own placement when mapping a hidden
    // window. Center the decorated frame after showing it, using a conservative
    // title-bar allowance if the platform cannot report its borders yet.
    WindowBorders borders = {48, 8, 8, 8};
    if (SDL_GetWindowBordersSize(s_window, &borders.top, &borders.left, &borders.bottom, &borders.right) != 0)
        borders = {48, 8, 8, 8};
    SDL_SetWindowPosition(s_window,
        desktop.x + (desktop.w - layout.w - borders.left - borders.right) / 2 + borders.left,
        desktop.y + (desktop.h - layout.h - borders.top - borders.bottom) / 2 + borders.top);
    // Apply the launcher's choice after normal placement so fullscreen uses
    // the same monitor. Desktop fullscreen keeps the display's native mode;
    // rendering quality and aspect ratio are still handled by the renderer.
    // Exclusive fullscreen switches the display to the mode asked for.
    if (const char* e = getenv("SMS_FULLSCREEN")) s_exclusive = strcmp(e, "exclusive") == 0;
    if (envTrue("SMS_FULLSCREEN")) setFullscreen(true);
    if (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN)
        logmsg("%s fullscreen on display %d (%s), internal resolution scale %d, OpenGL context ready",
               s_exclusive ? "exclusive" : "desktop", display, s_videoDriver, scale);
    else
        logmsg("window %dx%d centered on display %d (%s), internal resolution scale %d, OpenGL context ready",
               layout.w, layout.h, display, s_videoDriver, scale);
    s_windowFailure = WF_NONE;
    s_mouseCamera = envTrue("SMS_MOUSE_CAMERA");
    if (s_mouseCamera) {
        logmsg("mouse look on (F10 releases the mouse)");
        captureMouse((SDL_GetWindowFlags(s_window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
    return true;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// A program started without DISPLAY and WAYLAND_DISPLAY (by some desktop
// launchers, services and sandboxes) still has the user's desktop when its
// sockets are there: point SDL at them instead of deciding there is no screen.
void findDesktop() {
    const char* d = getenv("DISPLAY");
    const char* w = getenv("WAYLAND_DISPLAY");
    if ((d && *d) || (w && *w)) return;
    struct stat st;
    std::string runtime;
    if (const char* x = getenv("XDG_RUNTIME_DIR")) runtime = x;
    if (runtime.empty()) {
        char buf[64];
        snprintf(buf, sizeof buf, "/run/user/%u", (unsigned)getuid());
        runtime = buf;
        if (stat(runtime.c_str(), &st) == 0) setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    }
    if (stat((runtime + "/wayland-0").c_str(), &st) == 0) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        logmsg("WAYLAND_DISPLAY was not set; using the desktop's Wayland socket %s/wayland-0", runtime.c_str());
    }
    if (stat("/tmp/.X11-unix/X0", &st) == 0) {
        setenv("DISPLAY", ":0", 1);
        logmsg("DISPLAY was not set; using the desktop's X11 display :0");
    }
}

// SDL_VIDEODRIVER for SDL2, SDL_VIDEO_DRIVER for SDL3 (and sdl2-compat on it)
void forceVideoDriver(const char* name) {
    setenv("SDL_VIDEODRIVER", name, 1);
    setenv("SDL_VIDEO_DRIVER", name, 1);
}
#endif

// openWindow, and when a display's window fails, again on its other backend
// (Wayland and X11: a desktop session usually has both, and a driver or
// library problem often affects only one).
bool openWindowAnyBackend(int scale) {
#if !defined(_WIN32) && !defined(__APPLE__)
    findDesktop();
#endif
    if (openWindow(scale)) return true;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (s_windowFailure != WF_WINDOW) return false;
    const char* other = !strcmp(s_videoDriver, "wayland") ? "x11" : !strcmp(s_videoDriver, "x11") ? "wayland" : nullptr;
    if (other && getenv(!strcmp(other, "x11") ? "DISPLAY" : "WAYLAND_DISPLAY")) {
        const std::string first = s_videoDriver, firstError = s_windowError;
        logmsg("no window on %s (%s); trying %s", first.c_str(), firstError.c_str(), other);
        forceVideoDriver(other);
        if (openWindow(scale)) return true;
        snprintf(s_windowError, sizeof s_windowError, "%s: %s; %s: %s", first.c_str(), firstError.c_str(), other,
                 std::string(s_windowError).c_str());
        s_windowFailure = WF_WINDOW;
    }
#endif
    return false;
}
#endif

#ifdef SMS_GX_HAVE_EGL
void* eglGetProc(const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

bool tryEglDisplay(EGLDisplay dpy) {
    EGLint major, minor;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) return false;
    if (!eglBindAPI(EGL_OPENGL_API)) return false;
    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) return false;
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) return false;
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return true;
    // no surfaceless support: fall back to a tiny pbuffer
    const EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    return pb != EGL_NO_SURFACE && eglMakeCurrent(dpy, pb, pb, ctx);
}

bool openHeadless(int scale) {
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    bool ok = false;
    // Mesa crashes initialising a hardware EGL device when software rendering
    // is forced; go straight to the surfaceless platform then.
    if (getPlatformDisplay && queryDevices && !envTrue("LIBGL_ALWAYS_SOFTWARE")) {
        EGLDeviceEXT devs[8];
        EGLint n = 0;
        if (queryDevices(8, devs, &n))
            for (EGLint i = 0; i < n && !ok; i++) ok = tryEglDisplay(getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr));
    }
    if (!ok && getPlatformDisplay)
        ok = tryEglDisplay(getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr));
    if (!ok) ok = tryEglDisplay(eglGetDisplay(EGL_DEFAULT_DISPLAY));
    if (!ok) {
        logmsg("headless: no EGL OpenGL 3.3 context (try LIBGL_ALWAYS_SOFTWARE=1)");
        return false;
    }
    return GXPC_Init(eglGetProc, scale) != 0;
}
#endif

void dumpFrame(const void* xfb) {
    static int every = -1;
    static const char* dir = nullptr;
    if (every < 0) {
        const char* e = getenv("SMS_GX_DUMP_EVERY");
        every = e ? atoi(e) : 0;
        dir = getenv("SMS_GX_DUMP_DIR");
        if (!dir) dir = ".";
    }
    if (every <= 0 || s_frame % uint32_t(every) != 0) return;
    int w = 0, h = 0;
    if (!GXPC_ReadXFB(xfb, nullptr, &w, &h)) return;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    GXPC_ReadXFB(xfb, px.data(), &w, &h);
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06u.ppm", dir, s_frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) fwrite(&px[size_t(i) * 4], 1, 3, f);
    fclose(f);
}

void onDisplayCopy(const void* xfb) {
    s_frame++;
    GXPC_OverlayFrame();
    dumpFrame(xfb);
    const bool skip = s_skipPresent && s_presentCost >= kSkipPresentCost;
    s_skipPresent = false;
    s_lastPresent = 0;
    if (!s_autoPresent) return;
    if (skip) sms_gx_pump_events();
    else GXPC_Present(xfb);
}
}  // namespace

extern "C" {

int GXPC_ParseArgs(int* argc, char** argv) {
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--headless") == 0) s_forceHeadless = 1;
        else if (strcmp(a, "--window") == 0) s_forceHeadless = 0;
        else if (strcmp(a, "--vsync") == 0) s_vsync = 1;
        else {
            argv[out++] = argv[i];
            continue;
        }
    }
    int removed = *argc - out;
    *argc = out;
    argv[out] = nullptr;
    return removed;
}

void GXPC_SetHeadless(int headless) { s_forceHeadless = headless ? 1 : 0; }

void GXPC_SetWindowIcon(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    s_icon.assign(rgba, rgba + (size_t)w * h * 4);
    s_iconW = w;
    s_iconH = h;
#ifdef SMS_GX_HAVE_SDL2
    applyIcon();
#endif
}
void GXPC_SetAutoPresent(int enable) { s_autoPresent = enable != 0; }
void GXPC_SkipNextPresent(int skip) { s_skipPresent = skip != 0; }
double GXPC_LastPresentSeconds(void) { return s_lastPresent; }
int GXPC_MouseCaptured(void) {
#ifdef SMS_GX_HAVE_SDL2
    return s_mouseCaptured;
#else
    return 0;
#endif
}
int GXPC_IsHeadless(void) { return s_mode != MODE_WINDOW; }
uint32_t GXPC_FrameCount(void) { return s_frame; }

int GXPC_InitAuto(int efbScale) {
    if (rendererReady()) return 1;
    if (const char* e = getenv("SMS_GX_SCALE")) efbScale = atoi(e) > 0 ? atoi(e) : efbScale;
    if (envTrue("SMS_VSYNC")) s_vsync = strcmp(getenv("SMS_VSYNC"), "adaptive") == 0 ? -1 : 1;
    bool headless;
    if (s_forceHeadless >= 0) headless = s_forceHeadless != 0;
    else if (envTrue("SMS_HEADLESS")) headless = true;
    // Otherwise a window: SDL finds out whether there is a display (see
    // findDesktop for a session that lost DISPLAY / WAYLAND_DISPLAY).
    else headless = false;
    g_displayCopyHook = onDisplayCopy;
#ifdef SMS_GX_HAVE_SDL2
    if (!headless) {
        if (openWindowAnyBackend(efbScale)) {
            s_mode = MODE_WINDOW;
            return 1;
        }
        if (s_windowFailure == WF_WINDOW) {
            // A display is there but no window could be made. Running on
            // invisibly (sound, no picture) would leave the player nothing to
            // close; say why and stop instead. --headless / SMS_HEADLESS=1
            // still run without a window.
            char msg[512];
            snprintf(msg, sizeof msg,
                     "Super Mario Sunshine could not open its window (%s).\n\nUpdating your graphics drivers, or "
                     "setting SDL_VIDEODRIVER=x11 or =wayland, may help.",
                     s_windowError);
            logmsg("no window could be opened on this display (%s); exiting", s_windowError);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario Sunshine", msg, nullptr);
            exit(1);
        }
        logmsg("no display (%s), continuing headless", s_windowError);
    }
#endif
#ifdef SMS_GX_HAVE_EGL
    if (openHeadless(efbScale)) {
        s_mode = MODE_HEADLESS;
        logmsg("headless OpenGL context ready");
        return 1;
    }
#endif
    (void)headless;
    logmsg("no OpenGL context could be created; rendering is disabled");
    return 0;
}

void GXPC_Present(const void* xfb) {
#ifdef SMS_GX_HAVE_SDL2
    if (s_mode == MODE_WINDOW && s_window) {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(s_window, &w, &h);
        double t0 = nowSeconds();
        GXPC_PresentXFB(xfb, w, h);
        GXPC_OverlayDraw(w, h);
        double t1 = nowSeconds();
        SDL_GL_SwapWindow(s_window);
        waitSimulatedRefresh();
        double t2 = nowSeconds();
        g_presentSeconds += t1 - t0;
        g_swapSeconds += t2 - t1;
        s_lastPresent = t2 - t0;
        s_presentCost += (s_lastPresent - s_presentCost) * 0.1;
        GXPC_EndPresent();
        sms_gx_pump_events();
    }
#else
    (void)xfb;
#endif
}

void sms_gx_set_event_callback(void (*cb)(const union SDL_Event*)) {
#ifdef SMS_GX_HAVE_SDL2
    s_eventCb = cb;
#else
    (void)cb;
#endif
}

void sms_gx_pump_events(void) {
#ifdef SMS_GX_HAVE_SDL2
    if (s_mode != MODE_WINDOW) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_IsGameController(ev.cdevice.which)) {
                if (SDL_GameController* c = SDL_GameControllerOpen(ev.cdevice.which)) s_pads.push_back(c);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (size_t i = 0; i < s_pads.size(); i++)
                if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == ev.cdevice.which) {
                    SDL_GameControllerClose(s_pads[i]);
                    s_pads.erase(s_pads.begin() + long(i));
                    break;
                }
            break;
        default:
            break;
        }
        // backtick toggles the debug overlay and is kept from the pad layer
        if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && ev.key.keysym.scancode == SDL_SCANCODE_GRAVE) {
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) GXPC_OverlayToggle();
            continue;
        }
        if (s_mouseCamera) {
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) captureMouse(false);
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && !s_mouseReleased)
                captureMouse(true);
            if (ev.type == SDL_MOUSEBUTTONDOWN && !s_mouseCaptured) {
                s_mouseReleased = false;
                captureMouse(true);
                continue;
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F10) {
                if (!ev.key.repeat) {
                    s_mouseReleased = s_mouseCaptured;
                    captureMouse(!s_mouseCaptured);
                }
                continue;
            }
        }
        // F11 or Alt+Enter toggles fullscreen and is kept from the pad layer
        if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) &&
            (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
             ((ev.key.keysym.scancode == SDL_SCANCODE_RETURN || ev.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) &&
              (ev.key.keysym.mod & KMOD_ALT)))) {
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) setFullscreen(!s_isFullscreen);
            continue;
        }
        // F7 with the overlay open cycles the game speed
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F7 && GXPC_OverlayVisible()) {
            if (!ev.key.repeat) GXPC_CycleSpeed();
            continue;
        }
        if (s_eventCb) s_eventCb(&ev);
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            logmsg("window closed, exiting");
            GXPC_Shutdown();
            exit(0);
        }
    }
#endif
}

}  // extern "C"
