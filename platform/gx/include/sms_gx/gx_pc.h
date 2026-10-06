/* Host-side entry points of sms_gx that have no GameCube equivalent.
 *
 * The GX and GD API itself is the one declared by decomp/libs/dolphin/include/dolphin/gx*.h
 * and gd*.h; sms_gx implements those symbols with C linkage.  This header only
 * adds what a PC host needs: context bring-up, the physical-address window,
 * cache-flush hooks, the write-gather pipe and XFB presentation.
 */
#ifndef SMS_GX_GX_PC_H
#define SMS_GX_GX_PC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*GXPCGetProcFn)(const char* name);

/* ---- Window / context (gx_platform.cpp) ------------------------------------
 * GXInit calls GXPC_InitAuto(1) if no context exists yet, so the game needs no
 * changes: by default an SDL2 window with an OpenGL 3.3 core context opens when
 * a display is available (DISPLAY / WAYLAND_DISPLAY), otherwise an offscreen
 * EGL context is used.  Switches, checked in this order:
 *   GXPC_SetHeadless(1) or --headless (GXPC_ParseArgs)   force offscreen
 *   GXPC_SetHeadless(0) or --window                      force a window
 *   SMS_HEADLESS=1                                       force offscreen
 * Other environment: SMS_GX_SCALE=n (internal EFB scale), SMS_WINDOW_SCALE=n
 * (preferred window size multiple of 640x480, fitted to the desktop). By default
 * the window is at most 1280x720, fitted and centered in the usable desktop on
 * the pointer's display; render scale does not change its size.
 * SMS_VSYNC=1 or --vsync (swap interval 1),
 * SMS_OVERLAY=1 (open the debug overlay at start),
 * SMS_GX_DUMP_EVERY=n + SMS_GX_DUMP_DIR=dir (write every n-th XFB as PPM).
 * Every GXCopyDisp presents the copied XFB to the window and swaps (disable
 * with GXPC_SetAutoPresent(0) if the VI layer calls GXPC_Present itself). */
int GXPC_ParseArgs(int* argc, char** argv);  /* strips the options above; returns count removed */
void GXPC_SetHeadless(int headless);
int GXPC_InitAuto(int efbScale);             /* 1 on success */
int GXPC_IsHeadless(void);
void GXPC_SetAutoPresent(int enable);
void GXPC_Present(const void* xfb);          /* window mode: draw XFB, swap, pump events */
uint32_t GXPC_FrameCount(void);              /* display copies so far */
/* Window (taskbar / Dock) icon: w x h RGBA8 pixels, copied. Applied to the
 * SDL window now, or when it opens; ignored without a window. */
void GXPC_SetWindowIcon(const uint8_t* rgba, int w, int h);
/* 1 while the window holds the mouse for mouse look (SMS_MOUSE_CAMERA). */
int GXPC_MouseCaptured(void);
/* Widescreen: the displayed width over the GameCube's 4:3 (1 = off). Set
 * before the context exists; the EFB, the display and the window widen, and
 * draws map the game's 640-wide coordinates into it (see gx_render.cpp). The
 * game camera itself must be widened by the same factor to fill it. */
void GXPC_SetWidescreen(float widthOver43);
/* With widescreen, draw the following 2D stretched across the whole frame
 * (the game's faders call it around their drawing); 0 goes back. */
void GXPC_SetStretch2D(int on);
/* SMS_WIDESCREEN_HUD=edges: mark the gameplay HUD's drawing, and bracket
 * each J2D pane's drawing with its horizontal extent (game x), so HUD pieces
 * move to the screen edge on their side. */
void GXPC_SetHud(int on);
void GXPC_HudPaneBegin(float x1, float x2);
void GXPC_HudPaneEnd(void);
float GXPC_GetWidescreen(void);

/* Input hook for the PAD layer.  sms_gx_pump_events runs SDL_PollEvent (it is
 * also called after every present), opens game controllers as they appear and
 * hands every event to the registered callback.  Closing the window exits the
 * process.  The callback receives SDL2's `const SDL_Event*`. */
union SDL_Event;
void sms_gx_set_event_callback(void (*cb)(const union SDL_Event* ev));
void sms_gx_pump_events(void);

/* Call once with an OpenGL 3.3 core context current (SDL, EGL, ...), before the
 * game calls GXInit.  efbScale multiplies the 640x528 EFB (1 = native).
 * Returns 0 on failure (missing entry points, FBO creation failed). */
int GXPC_Init(GXPCGetProcFn getProc, int efbScale);
void GXPC_Shutdown(void);

/* Before endian conversion, discover HD replacements in BTI, J3D TEX1 and
 * JPA resources. Thread-safe; hashes synchronously and keeps no game pointers.
 * PreloadTextures drains those requests on the GL thread before gameplay. */
void GXPC_PrefetchResource(const void* data, uint32_t size, const char* name);
void GXPC_PreloadTextures(void);

/* 32-bit "physical" addresses appear inside command streams: display lists
 * built by GD/J3D carry texture, TLUT, vertex-array and EFB-copy addresses.
 * sms_gx maps them as phys = ptr - base.  The default base is 0x80000000,
 * matching the unmodified OSCachedToPhysical macro and a MEM1 arena mapped at
 * the GameCube address; call this only if emulated MEM1 lives elsewhere, and
 * keep OSCachedToPhysical consistent with it.  API calls (GXInitTexObj,
 * GXSetArray, GXCopyTex, ...) keep full host pointers and work for memory
 * outside the window too. */
void GXPC_SetMemoryWindow(void* base, uint32_t size);
uint32_t GXPC_PtrToPhys(const void* ptr);
void* GXPC_PhysToPtr(uint32_t phys);

/* Cache hooks: call from DCFlushRange/DCStoreRange (and DCInvalidateRange) so
 * textures the CPU rewrote are re-decoded.  GXInvalidateTexAll re-checks every
 * cached texture's contents as well. */
void GXPC_InvalidateRange(const void* ptr, uint32_t size);

/* Byte order of indexed vertex / matrix arrays (GXSetArray).  Display lists and
 * pipe data are always big-endian; arrays default to host order because most
 * are written by host code.  Arrays kept in their on-disc byte order (e.g. raw
 * BMD vertex data) must be flagged big-endian.  attr is a GXAttr value
 * (GX_VA_POS..GX_VA_TEX7, GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY, GX_VA_NBT). */
void GXPC_SetArrayBigEndian(int attr, int bigEndian);
void GXPC_SetDefaultArrayBigEndian(int bigEndian);
/* Preferred: tell sms_gx which memory holds on-disc (big-endian) data, e.g.
 * every resource file loaded in place.  GXSetArray / CP array bases that point
 * into a registered range are then read big-endian automatically. */
void GXPC_AddBigEndianRange(const void* ptr, uint32_t size);
void GXPC_RemoveBigEndianRange(const void* ptr);

/* The write-gather pipe (0xCC008000).  GXVert.h's inline writers and direct
 * `GXWGFifo.u8 = ...` stores must end up here on PC; see README.md. */
void GXPC_Write8(uint8_t v);
void GXPC_Write16(uint16_t v);
void GXPC_Write32(uint32_t v);
void GXPC_WriteF32(float v);

/* Presentation.  GXCopyDisp(dest, ...) records the EFB region as the XFB at
 * `dest`.  PresentXFB draws that XFB into the currently bound default
 * framebuffer (window size winW x winH, letterboxed) - the VI layer calls it
 * on retrace for the buffer given to VISetNextFrameBuffer, then swaps.
 * xfb == NULL presents the most recent display copy.  Returns 0 if unknown. */
int GXPC_PresentXFB(const void* xfb, int winW, int winH);
void GXPC_EndPresent(void);  /* after the swap: rebind the EFB (PresentXFB leaves framebuffer 0 bound) */

/* Debug/test access: read back the EFB (RGBA8, top row first) or the XFB
 * texture at `xfb` (NULL = most recent).  Buffers are w*h*4 bytes; pass NULL to
 * query the size only. */
void GXPC_ReadEFB(uint8_t* rgba, int* w, int* h);
int GXPC_ReadXFB(const void* xfb, uint8_t* rgba, int* w, int* h);

/* Statistics for the current frame (reset by GXCopyDisp). */
typedef struct GXPCStats {
    uint32_t draws, vertices, shaderCompiles, textureUploads, efbCopies;
} GXPCStats;
void GXPC_GetStats(GXPCStats* out);
void GXPC_GetLastFrameStats(GXPCStats* out); /* the last completed display frame */
double GXPC_GxSeconds(void);                   /* wall time spent inside sms_gx so far */
/* Where the game thread's time goes, as seconds since start. gx is all time
 * inside sms_gx apart from presenting; the parts of it (vertices .. gpuWait)
 * are measured only while GXPC_SetDetailedTimers(1) is on (the overlay turns
 * it on, as does SMS_GX_STATS). gpuWait is time blocked on the GPU (fences,
 * reading back copies, peeks and pixel counts); present is drawing the XFB to
 * the window, swap is SDL_GL_SwapWindow. idle is the time the game had no
 * runnable thread (waiting for the next retrace), from GXPC_SetIdleClock. */
typedef struct GXPCTimes {
    double gx, vertices, draws, textures, copies, peeks, gpuWait, present, swap, idle;
} GXPCTimes;
void GXPC_GetTimes(GXPCTimes* out);
void GXPC_SetDetailedTimers(int on);
void GXPC_SetIdleClock(double (*idleSeconds)(void));
/* Blend an RGBA image (row 0 = top) onto the window at (x, y) from its top-left,
 * magnified by scale.  Call between GXPC_PresentXFB and the swap. */
void GXPC_DrawOverlay(const uint8_t* rgba, int w, int h, int x, int y, int scale, int winW, int winH);
/* Debug overlay (backtick in the window): toggle, and draw it before a swap. */
void GXPC_OverlayToggle(void);
void GXPC_OverlayDraw(int winW, int winH);
int GXPC_OverlayVisible(void);
/* Game speed multiplier (F7 while the overlay is open cycles 1, 2, 4, 10);
 * VI retraces and host audio advance at this rate, including THP movies. */
void GXPC_CycleSpeed(void);
int GXPC_GetSpeed(void);

#ifdef __cplusplus
}

/* C++ replacement for the write-gather pipe union: `GXWGFifo.f32 = x` becomes
 * a call into the pipe.  To use it, GXVert.h (TARGET_PC) does
 *     #define GXWGFifo GXPC_WGPipe
 * instead of the 0xCC008000 cast. */
struct GXPCWGPipe {
    struct U8 { void operator=(uint8_t v) const { GXPC_Write8(v); } } u8;
    struct S8 { void operator=(int8_t v) const { GXPC_Write8((uint8_t)v); } } s8;
    struct U16 { void operator=(uint16_t v) const { GXPC_Write16(v); } } u16;
    struct S16 { void operator=(int16_t v) const { GXPC_Write16((uint16_t)v); } } s16;
    struct U32 { void operator=(uint32_t v) const { GXPC_Write32(v); } } u32;
    struct S32 { void operator=(int32_t v) const { GXPC_Write32((uint32_t)v); } } s32;
    struct F32 { void operator=(float v) const { GXPC_WriteF32(v); } } f32;
};
extern const GXPCWGPipe GXPC_WGPipe;
#endif

#endif
