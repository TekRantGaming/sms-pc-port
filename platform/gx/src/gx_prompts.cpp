// Button prompts for other controllers: the system font's eight button glyphs
// (A B X Y Z L R and the C-stick) drawn as the buttons of the controller being
// used. SMS_BUTTON_PROMPTS picks the style: gamecube (the game's own glyphs,
// the default), xbox, playstation, steamdeck, keyboard, or auto, which follows
// the last device used (keyboard and mouse, or the controller type SDL
// reports). With auto, SMS_BUTTON_PROMPT_PAD (gamecube, xbox, playstation or
// steamdeck) picks how every controller's prompts look, whatever the
// controller; unset or match follows the controller. The glyph images come
// from SMS_BUTTON_PROMPT_DIR/<style>.png: eight 64x64 cells in a row, in the
// order A B X Y Z L R C. SMS Launcher draws them from the player's bindings, so
// remapped keys and buttons show as bound.
//
// JUTResFont::drawChar_scale (decomp patch zzz-prompts-01) asks
// port_button_glyph for each character of the system font and draws the
// returned GX RGBA8 image in place of the glyph cell when there is one. The
// HUD's own button pictures (the FLUDD gauge's X) are swapped by the texture
// cache, which asks gx::promptImage (gx_texture.cpp).
#include "gx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third_party/stb_image.h"

// The GX images go in a GXTexObj, which holds 32-bit addresses on 64-bit hosts
// (PTR32), so they need memory below 4 GiB: the port runtime's
// port_low_alloc. The standalone GX tests link without the runtime, and their
// GXTexObj holds whole pointers, so this weak one stands in there.
extern "C" __attribute__((weak)) void* port_low_alloc(unsigned long size) { return malloc(size); }

namespace {

enum Style { S_GAMECUBE, S_XBOX, S_PLAYSTATION, S_STEAMDECK, S_KEYBOARD, S_COUNT };
const char* const kStyleNames[S_COUNT] = {"gamecube", "xbox", "playstation", "steamdeck", "keyboard"};
const int kCell = 64, kGlyphs = 8;

struct Atlas {
    bool tried = false, ok = false;
    uint8_t* tex[kGlyphs] = {};          // GX RGBA8, 64x64 each, below 4 GiB
    std::vector<uint8_t> rgba[kGlyphs];  // the same, row-major RGBA
};

bool s_inited, s_auto;
int s_fixed = S_GAMECUBE;
// auto's style for every controller, or -1 to match the controller's kind
int s_pad = -1;
std::string s_dir;
Atlas s_atlas[S_COUNT];
// the last device used, for auto: keyboard/mouse, or a controller's style
int s_device = S_KEYBOARD;

void init() {
    if (s_inited) return;
    s_inited = true;
    const char* e = getenv("SMS_BUTTON_PROMPTS");
    if (const char* d = getenv("SMS_BUTTON_PROMPT_DIR")) s_dir = d;
    if (!e || !*e) return;
    if (!strcmp(e, "auto")) s_auto = true;
    for (int i = 0; i < S_COUNT; i++)
        if (!strcmp(e, kStyleNames[i])) s_fixed = i;
    if (const char* p = getenv("SMS_BUTTON_PROMPT_PAD"))
        for (int i = S_GAMECUBE; i <= S_STEAMDECK; i++)
            if (!strcmp(p, kStyleNames[i])) s_pad = i;
    if (s_auto) gx::logmsg("button prompts: automatic, controllers as %s", s_pad < 0 ? "matched" : kStyleNames[s_pad]);
    else gx::logmsg("button prompts: %s", kStyleNames[s_fixed]);
}

// RGBA pixels (row-major) to the GX RGBA8 tiled layout: 4x4 blocks, each an
// AR half (16 A,R pairs) then a GB half.
void toGxRgba8(const uint8_t* rgba, int stride, uint8_t* out) {
    size_t o = 0;
    for (int by = 0; by < kCell; by += 4)
        for (int bx = 0; bx < kCell; bx += 4) {
            for (int half = 0; half < 2; half++)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const uint8_t* p = rgba + (by + y) * stride + (bx + x) * 4;
                        out[o++] = half ? p[1] : p[3];
                        out[o++] = half ? p[2] : p[0];
                    }
        }
}

const Atlas* atlas(int style) {
    Atlas& a = s_atlas[style];
    if (a.tried) return a.ok ? &a : nullptr;
    a.tried = true;
    if (s_dir.empty()) return nullptr;
    const std::string path = s_dir + "/" + kStyleNames[style] + ".png";
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) {
        gx::logmsg("button prompts: cannot read %s", path.c_str());
        return nullptr;
    }
    if (w < kCell * kGlyphs || h < kCell) {
        gx::logmsg("button prompts: %s is %dx%d, expected %dx%d", path.c_str(), w, h, kCell * kGlyphs, kCell);
        stbi_image_free(px);
        return nullptr;
    }
    const size_t cellBytes = kCell * kCell * 4;
    uint8_t* low = (uint8_t*)port_low_alloc(cellBytes * kGlyphs);
    if (!low) {
        gx::logmsg("button prompts: no memory below 4 GiB for %s", path.c_str());
        stbi_image_free(px);
        return nullptr;
    }
    for (int i = 0; i < kGlyphs; i++) {
        a.tex[i] = low + i * cellBytes;
        toGxRgba8(px + i * kCell * 4, w * 4, a.tex[i]);
        a.rgba[i].resize(kCell * kCell * 4);
        for (int y = 0; y < kCell; y++)
            memcpy(&a.rgba[i][y * kCell * 4], px + y * w * 4 + i * kCell * 4, kCell * 4);
    }
    stbi_image_free(px);
    a.ok = true;
    gx::logmsg("button prompts: loaded %s", path.c_str());
    return &a;
}

// the glyph index for a system-font character code, -1 for other characters
int glyphIndex(int chr) {
    switch (chr & 0xFF) {
    case '@': return 0;   // A
    case '#': return 1;   // B
    case '+': return 2;   // X
    case 0xA5: return 3;  // Y
    case '$': return 4;   // Z
    case '<': return 5;   // L
    case '>': return 6;   // R
    case '%': return 7;   // C-stick
    default: return -1;
    }
}

// the style shown now: the fixed one, or auto's for the last device used
int currentStyle() {
    init();
    if (!s_auto) return s_fixed;
    return s_device != S_KEYBOARD && s_pad >= 0 ? s_pad : s_device;
}

}  // namespace

namespace gx {

const uint8_t* promptImage(int glyph, int* style) {
    const int s = currentStyle();
    if (s == S_GAMECUBE || glyph < 0 || glyph >= kGlyphs) return nullptr;
    const Atlas* a = atlas(s);
    if (!a) return nullptr;
    *style = s;
    return a->rgba[glyph].data();
}

}  // namespace gx

extern "C" {

// From the GX layer's event loop: the device the player last used, keyboard
// and mouse (controller 0) or a controller with its SDL type and name.
void GXPC_NoteInputDevice(int controller, int sdlType, const char* name) {
    int style = S_KEYBOARD;
    if (controller) {
        // SDL_GameControllerType: 3 PS3, 4 PS4, 7 PS5; the rest use Xbox prompts.
        // SDL 2 has no Steam Deck type, so the Deck's own controls go by name.
        if (sdlType == 3 || sdlType == 4 || sdlType == 7) style = S_PLAYSTATION;
        else if (name && strstr(name, "Steam Deck")) style = S_STEAMDECK;
        else style = S_XBOX;
    }
    if (style != s_device) {
        s_device = style;
        if (s_auto) gx::logmsg("button prompts: now %s", kStyleNames[style]);
    }
}

// A GX RGBA8 64x64 image to draw in place of system-font character chr, or
// null to draw the font's own glyph.
const void* port_button_glyph(int chr) {
    init();
    const int g = glyphIndex(chr);
    if (g < 0) return nullptr;
    const int style = currentStyle();
    if (style == S_GAMECUBE) return nullptr;
    const Atlas* a = atlas(style);
    return a ? a->tex[g] : nullptr;
}

}
