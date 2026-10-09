// HDR output on Windows.
//
// OpenGL cannot present HDR on Windows, so with SMS_HDR=1, while Windows HDR
// is on for the game's monitor, a flip-model Direct3D 11 swap chain presents
// in the window's place, in scRGB (linear BT.709, 1.0 = 80 nits). The post
// passes draw each frame's SDR picture into a texture in place of the window
// (g_presentFbo), and Direct3D's shader (kHdrHlsl) expands it the way an
// SDR-to-HDR converter does: SDR white at the paper white, highlights
// stretched toward the display's peak, with contrast and saturation around
// them.
//
// The picture reaches Direct3D in a texture OpenGL shares with it through
// WGL_NV_DX_interop2. AMD's interop calls into Direct3D 10, so on AMD the
// device switches to a Direct3D 10 state while textures are registered (as
// Firefox does, gfx/gl/SharedSurfaceD3D11Interop.cpp). Where the driver shares
// nothing (no interop, or it refuses every texture), each frame is copied
// through system memory instead, a frame behind; SMS_HDR_COPY=1 forces that.
// When the window takes no swap chain (one already presents to it), the swap
// chain goes in a DirectComposition visual over the window.
//
// SMS_HDR_PAPER_WHITE and SMS_HDR_PEAK are in nits, or auto: Windows' "SDR
// content brightness", and the display's peak as Windows reports it through
// DXGI (which follows the Windows HDR Calibration app's profile when there is
// one). SMS_HDR_CONTRAST and SMS_HDR_SATURATION are percent (100 leaves them
// as they are); SMS_HDR_HIGHLIGHTS (0 to 100) is how far the brightest parts
// reach toward the peak, in stops: 0 keeps SDR white at the paper white, 50
// puts it halfway between the paper white and the peak, 100 at the peak.
// Anything that fails leaves the usual SDR window and says why in the log.
//
// --display-info (GXPC_PrintDisplayInfo) prints what Windows reports for each
// display as one line of JSON, for the launcher's HDR settings.
#include "gx_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d10.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#endif

#include "gl_funcs.h"
#include "gx_glcache.h"

namespace gx {
unsigned g_presentFbo = 0;
}

#ifdef _WIN32
namespace {
using gx::logmsg;

// WGL_NV_DX_interop2
typedef HANDLE(WINAPI* PFNOPEN)(void* dxDevice);
typedef BOOL(WINAPI* PFNCLOSE)(HANDLE device);
typedef HANDLE(WINAPI* PFNREGISTER)(HANDLE device, void* dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI* PFNUNREGISTER)(HANDLE device, HANDLE object);
typedef BOOL(WINAPI* PFNLOCK)(HANDLE device, GLint count, HANDLE* objects);
const GLenum WGL_ACCESS_READ_WRITE = 0x0001;
PFNOPEN wglDXOpenDevice;
PFNCLOSE wglDXCloseDevice;
PFNREGISTER wglDXRegisterObject;
PFNUNREGISTER wglDXUnregisterObject;
PFNLOCK wglDXLockObjects, wglDXUnlockObjects;

// ColorProfileGetDisplayDefault (mscms, Windows 11): the display's HDR profile,
// which the Windows HDR Calibration app sets.
typedef HRESULT(WINAPI* PFNPROFILE)(int scope, LUID adapter, UINT32 source, int type, int subtype, LPWSTR* name);
const int CPT_ICC_ = 0, CPST_EXTENDED_DISPLAY_COLOR_MODE_ = 8;

typedef HRESULT(WINAPI* PFNDCOMPOSITION)(IDXGIDevice* device, REFIID iid, void** out);

bool s_active;
ID3D11Device* s_dev;
ID3D11DeviceContext* s_ctx;
IDXGISwapChain3* s_chain;
ID3D11RenderTargetView* s_backView;  // the swap chain's back buffer
IDCompositionDevice* s_dcomp;        // when the swap chain is in a DirectComposition visual
IDCompositionTarget* s_dcompTarget;
IDCompositionVisual* s_dcompVisual;
ID3D11VertexShader* s_vs;
ID3D11PixelShader* s_ps;
ID3D11Buffer* s_params;
// The SDR picture: OpenGL draws it into s_pictureFbo, Direct3D reads s_picture.
ID3D11Texture2D* s_picture;
ID3D11ShaderResourceView* s_pictureView;
GLuint s_pictureTex, s_pictureFbo;
// Shared (WGL_NV_DX_interop2), with the Direct3D 10 state AMD's interop needs
HANDLE s_dxDevice, s_dxObject;
ID3D11DeviceContext1* s_ctx1;
ID3DDeviceContextState* s_d3d10State;
// or copied: each frame OpenGL reads the picture into one pixel buffer, and the
// other, a frame older and by now in system memory, goes to Direct3D.
bool s_copy;
GLuint s_pbo[2];
bool s_pboFilled[2];
int s_pboNext;
int s_w, s_h;
bool s_tearing;
float s_paper = 200, s_peak = 1000, s_contrast = 1, s_saturation = 1, s_highlights = 0.4f;

// The picture to scRGB, linear BT.709 with 1.0 at 80 nits. OpenGL wrote it
// bottom row first. It is decoded with a 2.2 gamma (the TVs the game was made
// for, and darker shadows than Windows' sRGB curve for SDR) after a dither of
// half an 8-bit step, so stretching it does not band. Contrast pivots on
// middle grey (18%); saturation keeps BT.709 luminance. Middle grey and below
// stay where SDR puts them (the paper white); brighter tones gain gradually,
// smoothly over the stops from middle grey to white, until SDR white is
// whiteGain times the paper white, so no narrow band of tones is stretched.
// The gain follows luminance, not the strongest channel, so a vivid colour (a
// blue menu panel) is not lifted as if it were white. Highlights (0..1) sets
// that gain in stops: 0 keeps SDR white at the paper white, 1 puts it at the
// peak. Each pixel is scaled as a whole, so its hue stays.
const char kHdrHlsl[] = R"(
cbuffer Params : register(b0) {
  float4 hdr;        // paper white, peak (scRGB units), contrast, saturation
  float4 whiteGain;  // x: SDR white over the paper white, at least 1
};
Texture2D<float4> picture : register(t0);

float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps(float4 pos : SV_Position) : SV_Target {
  uint w, h;
  picture.GetDimensions(w, h);
  float3 c = picture.Load(int3(int(pos.x), int(h) - 1 - int(pos.y), 0)).rgb;
  float dither = frac(sin(dot(pos.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
  c = pow(saturate(c + dither / 255.0), 2.2);
  c = 0.18 * pow(max(c, 0.0) / 0.18, hdr.z);
  float y = dot(c, float3(0.2126, 0.7152, 0.0722));
  c = max(lerp(float3(y, y, y), c, hdr.w), 0.0);
  float s = smoothstep(log2(0.18), 0.0, log2(max(y, 1e-6)));
  float3 o = c * (hdr.x * exp2(log2(whiteGain.x) * s));
  return float4(min(o, hdr.yyy), 1.0);
}
)";

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string utf8(const wchar_t* w) {
    std::string out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        out.resize(size_t(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    }
    return out;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

// What Windows' display configuration knows about the display whose GDI name
// is gdiName: its monitor name, the SDR content brightness (nits) and the HDR
// colour profile it uses.
struct ConfigInfo {
    std::string monitor, profile;
    float sdrWhite = 0;
};

ConfigInfo configInfo(const wchar_t* gdiName) {
    ConfigInfo info;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return info;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return info;
    for (UINT32 i = 0; i < pathCount; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiName) != 0)
            continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof target;
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) info.monitor = utf8(target.monitorFriendlyDeviceName);
        DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        // SDRWhiteLevel is 1000 for 80 nits
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) info.sdrWhite = float(white.SDRWhiteLevel) * 80.0f / 1000.0f;
        if (HMODULE mscms = LoadLibraryW(L"mscms.dll")) {
            auto getProfile = reinterpret_cast<PFNPROFILE>(reinterpret_cast<void*>(GetProcAddress(mscms, "ColorProfileGetDisplayDefault")));
            for (int scope = 1; getProfile && scope >= 0 && info.profile.empty(); scope--) {  // the user's, then the system's
                LPWSTR name = nullptr;
                if (SUCCEEDED(getProfile(scope, paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id, CPT_ICC_,
                                         CPST_EXTENDED_DISPLAY_COLOR_MODE_, &name)) && name) {
                    info.profile = utf8(name);
                    LocalFree(name);
                }
            }
        }
        break;
    }
    return info;
}

// Every display output with its HDR capabilities, as Windows reports them.
struct OutputInfo {
    IDXGIAdapter1* adapter = nullptr;
    HMONITOR monitor = nullptr;
    std::wstring gdiName;
    bool hdr = false;
    UINT bits = 0;
    float minNits = 0, peakNits = 0, fullFrameNits = 0;
};

std::vector<OutputInfo> outputs() {
    std::vector<OutputInfo> list;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return list;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            IDXGIOutput6* output6 = nullptr;
            DXGI_OUTPUT_DESC1 desc = {};
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
                SUCCEEDED(output6->GetDesc1(&desc))) {
                OutputInfo info;
                adapter->AddRef();
                info.adapter = adapter;
                info.monitor = desc.Monitor;
                info.gdiName = desc.DeviceName;
                info.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                info.bits = desc.BitsPerColor;
                info.minNits = desc.MinLuminance;
                info.peakNits = desc.MaxLuminance;
                info.fullFrameNits = desc.MaxFullFrameLuminance;
                list.push_back(info);
            }
            if (output6) output6->Release();
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return list;
}

void releaseOutputs(std::vector<OutputInfo>& list) {
    for (OutputInfo& o : list)
        if (o.adapter) o.adapter->Release();
    list.clear();
}

float envNits(const char* name, float fallback) {
    const char* e = getenv(name);
    if (!e || !*e || !strcmp(e, "auto")) return fallback;
    const float v = float(atof(e));
    return v >= 10.0f && v <= 10000.0f ? v : fallback;
}

float envPercent(const char* name, float lo, float hi, float fallback) {
    const char* e = getenv(name);
    if (!e || !*e) return fallback;
    const float v = float(atof(e));
    return v >= lo && v <= hi ? v / 100.0f : fallback;
}

// AMD's interop calls ID3D10Device::Flush, so the device is in its Direct3D 10
// state while shared textures are registered and unregistered.
struct D3D10State {
    ID3DDeviceContextState* previous = nullptr;
    D3D10State() {
        if (s_d3d10State) s_ctx1->SwapDeviceContextState(s_d3d10State, &previous);
    }
    ~D3D10State() {
        if (!s_d3d10State) return;
        s_ctx1->SwapDeviceContextState(previous, nullptr);
        release(previous);
    }
};

void releasePicture() {
    if (s_dxObject) {
        D3D10State state;
        wglDXUnregisterObject(s_dxDevice, s_dxObject);
    }
    s_dxObject = nullptr;
    release(s_pictureView);
    release(s_picture);
    if (s_pictureFbo) glDeleteFramebuffers(1, &s_pictureFbo);
    if (s_pictureTex) glDeleteTextures(1, &s_pictureTex);
    if (s_pbo[0]) glDeleteBuffers(2, s_pbo);
    s_pictureFbo = s_pictureTex = s_pbo[0] = s_pbo[1] = 0;
    s_pboFilled[0] = s_pboFilled[1] = false;
}

void releaseTargets() {
    releasePicture();
    if (s_ctx) s_ctx->ClearState();  // nothing may hold the back buffer when it is resized
    release(s_backView);
    s_w = s_h = 0;
}

void stop(const char* why) {
    logmsg("HDR: %s; the game shows in SDR", why);
    releaseTargets();
    if (s_dxDevice) wglDXCloseDevice(s_dxDevice);
    s_dxDevice = nullptr;
    release(s_d3d10State);
    release(s_ctx1);
    release(s_params);
    release(s_ps);
    release(s_vs);
    release(s_dcompVisual);
    release(s_dcompTarget);
    release(s_dcomp);
    release(s_chain);
    release(s_ctx);
    release(s_dev);
    s_active = false;
}

// The interop functions, and the interop device for s_dev. On AMD, first the
// Direct3D 10 state its interop needs.
bool openInterop(bool amd) {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    auto getProc = gl ? reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(reinterpret_cast<void*>(GetProcAddress(gl, "wglGetProcAddress")))
                      : nullptr;
    if (getProc) {
        wglDXOpenDevice = reinterpret_cast<PFNOPEN>(reinterpret_cast<void*>(getProc("wglDXOpenDeviceNV")));
        wglDXCloseDevice = reinterpret_cast<PFNCLOSE>(reinterpret_cast<void*>(getProc("wglDXCloseDeviceNV")));
        wglDXRegisterObject = reinterpret_cast<PFNREGISTER>(reinterpret_cast<void*>(getProc("wglDXRegisterObjectNV")));
        wglDXUnregisterObject = reinterpret_cast<PFNUNREGISTER>(reinterpret_cast<void*>(getProc("wglDXUnregisterObjectNV")));
        wglDXLockObjects = reinterpret_cast<PFNLOCK>(reinterpret_cast<void*>(getProc("wglDXLockObjectsNV")));
        wglDXUnlockObjects = reinterpret_cast<PFNLOCK>(reinterpret_cast<void*>(getProc("wglDXUnlockObjectsNV")));
    }
    if (!wglDXOpenDevice || !wglDXCloseDevice || !wglDXRegisterObject || !wglDXUnregisterObject || !wglDXLockObjects ||
        !wglDXUnlockObjects) {
        logmsg("HDR: the graphics driver has no WGL_NV_DX_interop2");
        return false;
    }
    if (amd) {
        ID3D11Device1* dev1 = nullptr;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
        if (SUCCEEDED(s_dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1))) &&
            SUCCEEDED(s_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&s_ctx1))))
            dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device), nullptr, &s_d3d10State);
        release(dev1);
        if (!s_d3d10State) logmsg("HDR: no Direct3D 10 state for AMD's interop; trying without");
    }
    s_dxDevice = wglDXOpenDevice(s_dev);
    if (!s_dxDevice) {
        logmsg("HDR: the graphics driver shares nothing between OpenGL and Direct3D (error %lu)", GetLastError());
        return false;
    }
    return true;
}

// A w x h texture OpenGL draws the picture into and Direct3D reads, shared
// through WGL_NV_DX_interop2. Drivers differ in what they share, so this tries
// BGRA then RGBA, each as a D3D11_RESOURCE_MISC_SHARED texture and a plain one.
bool makeShared(int w, int h) {
    static const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    static const UINT miscs[] = {D3D11_RESOURCE_MISC_SHARED, 0};
    static bool logged;
    DWORD error = 0;
    for (DXGI_FORMAT format : formats)
        for (UINT misc : miscs) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = UINT(w);
            desc.Height = UINT(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = misc;
            ID3D11Texture2D* tex = nullptr;
            if (FAILED(s_dev->CreateTexture2D(&desc, nullptr, &tex))) continue;
            GLuint name = 0;
            glGenTextures(1, &name);
            HANDLE object;
            {
                D3D10State state;
                object = wglDXRegisterObject(s_dxDevice, tex, name, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE);
            }
            if (!object) {
                error = GetLastError();
                glDeleteTextures(1, &name);
                tex->Release();
                continue;
            }
            s_picture = tex;
            s_pictureTex = name;
            s_dxObject = object;
            // OpenGL may use a shared texture only while it is locked
            if (!wglDXLockObjects(s_dxDevice, 1, &s_dxObject)) {
                logmsg("HDR: the shared texture could not be locked (error %lu)", GetLastError());
                return false;
            }
            glGenFramebuffers(1, &s_pictureFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s_pictureFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_pictureTex, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            wglDXUnlockObjects(s_dxDevice, 1, &s_dxObject);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                logmsg("HDR: OpenGL cannot draw into the shared texture (0x%x)", status);
                return false;
            }
            if (!logged)
                logmsg("HDR: OpenGL shares a %s%s texture with Direct3D", format == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "RGBA",
                       misc ? " D3D11_RESOURCE_MISC_SHARED" : "");
            logged = true;
            return true;
        }
    logmsg("HDR: the graphics driver will not share a texture with Direct3D (error %lu)", error);
    return false;
}

// The copy's textures: OpenGL's, which it draws the picture into and reads back
// into two pixel buffers, and Direct3D's, which the CPU writes.
bool makeCopy(int w, int h) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(s_dev->CreateTexture2D(&desc, nullptr, &s_picture))) return false;
    glGenTextures(1, &s_pictureTex);
    glBindTexture(GL_TEXTURE_2D, s_pictureTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &s_pictureFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_pictureFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_pictureTex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenBuffers(2, s_pbo);
    for (GLuint pbo : s_pbo) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(w) * h * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_pboNext = 0;
    return ok;
}

// The swap chain's buffers and the picture's textures, all w x h. A texture
// the driver will not share switches to the copy for the rest of the game.
bool makeTargets(int w, int h) {
    releaseTargets();
    HRESULT hr = s_chain->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                        s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(hr)) hr = s_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (SUCCEEDED(hr)) hr = s_dev->CreateRenderTargetView(back, nullptr, &s_backView);
    release(back);
    if (FAILED(hr)) {
        logmsg("HDR: the %dx%d swap chain buffers failed (0x%08lx)", w, h, hr);
        return false;
    }
    if (!s_copy && !makeShared(w, h)) {
        releasePicture();
        s_copy = true;
        logmsg("HDR: copying each frame to Direct3D through system memory instead, a frame behind");
    }
    bool ok = !s_copy || makeCopy(w, h);
    ok = ok && SUCCEEDED(s_dev->CreateShaderResourceView(s_picture, nullptr, &s_pictureView));
    gx::glcInvalidate();
    if (!ok) {
        logmsg("HDR: the %dx%d picture textures failed", w, h);
        return false;
    }
    s_w = w;
    s_h = h;
    return true;
}

// Reads this frame's picture into a pixel buffer and gives Direct3D the one
// from the frame before, which the GPU has finished by now, so neither waits.
// The first frame after a resize has no frame before and waits for its own.
void copyPicture() {
    const int now = s_pboNext, before = now ^ 1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_pictureFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[now]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s_w, s_h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    s_pboFilled[now] = true;
    const int ready = s_pboFilled[before] ? before : now;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[ready]);
    const size_t row = size_t(s_w) * 4;
    const char* src = static_cast<const char*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(row * size_t(s_h)), GL_MAP_READ_BIT));
    D3D11_MAPPED_SUBRESOURCE dst;
    if (src && SUCCEEDED(s_ctx->Map(s_picture, 0, D3D11_MAP_WRITE_DISCARD, 0, &dst))) {
        for (int y = 0; y < s_h; y++) memcpy(static_cast<char*>(dst.pData) + size_t(y) * dst.RowPitch, src + size_t(y) * row, row);
        s_ctx->Unmap(s_picture, 0);
    }
    if (src) glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    s_pboNext = before;
    gx::glcInvalidate();
}

// kHdrHlsl's shaders (d3dcompiler_47.dll is part of Windows), and its settings.
bool makeShaders() {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile"))) : nullptr;
    if (!compile) {
        logmsg("HDR: no d3dcompiler_47.dll");
        return false;
    }
    ID3DBlob* code[2] = {};
    static const char* const entry[2] = {"vs", "ps"};
    static const char* const target[2] = {"vs_4_0", "ps_4_0"};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        ID3DBlob* errors = nullptr;
        ok = SUCCEEDED(compile(kHdrHlsl, sizeof kHdrHlsl - 1, "kHdrHlsl", nullptr, nullptr, entry[i], target[i],
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code[i], &errors));
        if (!ok && errors) logmsg("HDR: %s", static_cast<const char*>(errors->GetBufferPointer()));
        release(errors);
    }
    ok = ok && SUCCEEDED(s_dev->CreateVertexShader(code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &s_vs));
    ok = ok && SUCCEEDED(s_dev->CreatePixelShader(code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &s_ps));
    release(code[0]);
    release(code[1]);
    const float params[8] = {s_paper / 80.0f, s_peak / 80.0f, s_contrast, s_saturation,
                             s_peak > s_paper ? powf(s_peak / s_paper, s_highlights) : 1.0f, 0, 0, 0};
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof params;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = params;
    return ok && SUCCEEDED(s_dev->CreateBuffer(&desc, &data, &s_params));
}

// A swap chain for a DirectComposition visual over the window, for a window
// that takes none of its own.
HRESULT compositionSwapChain(IDXGIFactory2* factory, HWND hwnd, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain1** chain) {
    HMODULE dll = LoadLibraryW(L"dcomp.dll");
    auto create =
        dll ? reinterpret_cast<PFNDCOMPOSITION>(reinterpret_cast<void*>(GetProcAddress(dll, "DCompositionCreateDevice"))) : nullptr;
    IDXGIDevice* dxgi = nullptr;
    HRESULT hr = create ? s_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = create(dxgi, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&s_dcomp));
    release(dxgi);
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForComposition(s_dev, &desc, nullptr, chain);
    if (SUCCEEDED(hr)) hr = s_dcomp->CreateTargetForHwnd(hwnd, TRUE, &s_dcompTarget);
    if (SUCCEEDED(hr)) hr = s_dcomp->CreateVisual(&s_dcompVisual);
    if (SUCCEEDED(hr)) hr = s_dcompVisual->SetContent(*chain);
    if (SUCCEEDED(hr)) hr = s_dcompTarget->SetRoot(s_dcompVisual);
    if (SUCCEEDED(hr)) hr = s_dcomp->Commit();
    return hr;
}

// The scRGB swap chain: the window's own, or else one in a DirectComposition
// visual over it.
bool makeSwapChain(HWND hwnd) {
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
        logmsg("HDR: no DXGI factory");
        return false;
    }
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
        BOOL allow = FALSE;
        s_tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)) && allow;
        factory5->Release();
    }
    RECT client = {};
    GetClientRect(hwnd, &client);
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(std::max<LONG>(1, client.right - client.left));
    desc.Height = UINT(std::max<LONG>(1, client.bottom - client.top));
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* chain = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(s_dev, hwnd, &desc, nullptr, nullptr, &chain);
    if (SUCCEEDED(hr)) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    } else {
        logmsg("HDR: the window takes no swap chain (0x%08lx); trying DirectComposition", hr);
        hr = compositionSwapChain(factory, hwnd, desc, &chain);
        if (FAILED(hr)) logmsg("HDR: no DirectComposition swap chain either (0x%08lx)", hr);
    }
    factory->Release();
    if (SUCCEEDED(hr)) hr = chain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s_chain));
    release(chain);
    return SUCCEEDED(hr);
}

}  // namespace

namespace gx {

bool hdrActive() { return s_active; }

bool hdrInit(void* window) {
    const char* want = getenv("SMS_HDR");
    if (!want || !*want || !strcmp(want, "0") || s_active) return false;
    HWND hwnd = static_cast<HWND>(window);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    std::vector<OutputInfo> list = outputs();
    const OutputInfo* out = nullptr;
    for (const OutputInfo& o : list)
        if (o.monitor == monitor) out = &o;
    if (!out || !out->hdr) {
        logmsg("HDR: Windows HDR is off for this monitor (Settings > Display > Use HDR); the game shows in SDR");
        releaseOutputs(list);
        return false;
    }
    const ConfigInfo config = configInfo(out->gdiName.c_str());
    s_paper = envNits("SMS_HDR_PAPER_WHITE", config.sdrWhite >= 80.0f ? config.sdrWhite : 200.0f);
    s_peak = envNits("SMS_HDR_PEAK", out->peakNits >= 100.0f ? out->peakNits : 1000.0f);
    if (s_peak < s_paper) s_peak = s_paper;
    s_contrast = envPercent("SMS_HDR_CONTRAST", 50, 150, 1.0f);
    s_saturation = envPercent("SMS_HDR_SATURATION", 0, 200, 1.0f);
    s_highlights = envPercent("SMS_HDR_HIGHLIGHTS", 0, 100, 0.4f);

    DXGI_ADAPTER_DESC1 adapter = {};
    out->adapter->GetDesc1(&adapter);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT made = D3D11CreateDevice(out->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                           2, D3D11_SDK_VERSION, &s_dev, nullptr, &s_ctx);
    releaseOutputs(list);
    if (FAILED(made)) {
        stop("no Direct3D 11 device");
        return false;
    }
    // WGL_NV_DX_interop2 may use the device from the driver's own threads
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(s_ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))))
        multithread->SetMultithreadProtected(TRUE);
    release(multithread);
    if (!makeShaders()) {
        stop("the HDR shader did not compile");
        return false;
    }
    const char* copy = getenv("SMS_HDR_COPY");
    s_copy = copy && *copy && strcmp(copy, "0") != 0;
    if (!s_copy && !openInterop(adapter.VendorId == 0x1002)) {
        s_copy = true;
        logmsg("HDR: copying each frame to Direct3D through system memory instead, a frame behind");
    }
    if (!makeSwapChain(hwnd)) {
        stop("no Direct3D swap chain for the window");
        return false;
    }
    if (FAILED(s_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
        stop("the swap chain cannot take scRGB");
        return false;
    }
    s_active = true;
    logmsg("HDR on: scRGB through Direct3D 11 on %s (%s%s), paper white %.0f nits, peak %.0f nits, contrast %.0f%%, "
           "saturation %.0f%%, highlights %.0f%% (SDR white at %.0f nits)%s",
           utf8(adapter.Description).c_str(), s_copy ? "copied" : "shared", s_dcomp ? ", DirectComposition" : "",
           double(s_paper), double(s_peak), double(s_contrast * 100), double(s_saturation * 100), double(s_highlights * 100),
           double(s_paper * powf(s_peak / s_paper, s_highlights)),
           config.profile.empty() ? "" : (", calibration profile " + config.profile).c_str());
    return true;
}

bool hdrFrameBegin(int w, int h) {
    if (!s_active || w <= 0 || h <= 0) return false;
    if ((w != s_w || h != s_h) && !makeTargets(w, h)) {
        stop("the frame's textures could not be made");
        return false;
    }
    if (s_dxObject && !wglDXLockObjects(s_dxDevice, 1, &s_dxObject)) {
        logmsg("HDR: the shared texture could not be locked (error %lu); copying each frame instead", GetLastError());
        releasePicture();
        s_copy = true;
        if (!makeTargets(w, h)) {
            stop("the frame's textures could not be made");
            return false;
        }
    }
    g_presentFbo = s_pictureFbo;
    return true;
}

void hdrFramePresent(int vsync) {
    g_presentFbo = 0;
    if (!s_active) return;
    if (s_dxObject) wglDXUnlockObjects(s_dxDevice, 1, &s_dxObject);
    else copyPicture();
    const D3D11_VIEWPORT viewport = {0, 0, float(s_w), float(s_h), 0, 1};
    s_ctx->OMSetRenderTargets(1, &s_backView, nullptr);
    s_ctx->RSSetViewports(1, &viewport);
    s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_ctx->VSSetShader(s_vs, nullptr, 0);
    s_ctx->PSSetShader(s_ps, nullptr, 0);
    s_ctx->PSSetConstantBuffers(0, 1, &s_params);
    s_ctx->PSSetShaderResources(0, 1, &s_pictureView);
    s_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    s_ctx->PSSetShaderResources(0, 1, &none);  // the shared texture goes back to OpenGL
    const UINT interval = vsync ? 1 : 0;
    s_chain->Present(interval, interval == 0 && s_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

}  // namespace gx

extern "C" void GXPC_PrintDisplayInfo(void) {
    std::vector<OutputInfo> list = outputs();
    std::string json = "{\"displays\":[";
    for (size_t i = 0; i < list.size(); i++) {
        const OutputInfo& o = list[i];
        const ConfigInfo c = configInfo(o.gdiName.c_str());
        char nums[256];
        snprintf(nums, sizeof nums,
                 ",\"hdr\":%s,\"bitsPerColor\":%u,\"minNits\":%.4f,\"peakNits\":%.0f,\"fullFrameNits\":%.0f,\"sdrWhiteNits\":%.0f",
                 o.hdr ? "true" : "false", o.bits, double(o.minNits), double(o.peakNits), double(o.fullFrameNits),
                 double(c.sdrWhite));
        if (i) json += ",";
        json += "{\"device\":" + jsonString(utf8(o.gdiName.c_str())) + ",\"monitor\":" + jsonString(c.monitor) + nums +
                ",\"calibrationProfile\":" + jsonString(c.profile) + "}";
    }
    json += "]}";
    releaseOutputs(list);
    printf("%s\n", json.c_str());
    fflush(stdout);
}

#else  // not Windows: no HDR output yet

namespace gx {
bool hdrActive() { return false; }
bool hdrInit(void*) {
    const char* want = getenv("SMS_HDR");
    if (want && *want && strcmp(want, "0") != 0) logmsg("HDR: only on Windows so far; the game shows in SDR");
    return false;
}
bool hdrFrameBegin(int, int) { return false; }
void hdrFramePresent(int) {}
}  // namespace gx

extern "C" void GXPC_PrintDisplayInfo(void) {
    printf("{\"displays\":[]}\n");
    fflush(stdout);
}

#endif
