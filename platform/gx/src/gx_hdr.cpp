// HDR output on Windows.
//
// OpenGL cannot present HDR on Windows, so with SMS_HDR=1, while Windows HDR
// is on for the game's monitor, the post passes draw each frame into an SDR
// texture in place of the window (g_presentFbo). gx::hdrPass turns that into
// scRGB (linear BT.709, 1.0 = 80 nits) in a texture shared with Direct3D 11
// through WGL_NV_DX_interop2, which a flip-model swap chain on the same window
// presents. Sunshine's art is SDR, so hdrPass expands it the way an SDR-to-HDR
// converter does: SDR white at the paper white, highlights stretched toward the
// display's peak, with contrast and saturation around them.
//
// SMS_HDR_PAPER_WHITE and SMS_HDR_PEAK are in nits, or auto: Windows' "SDR
// content brightness", and the display's peak as Windows reports it through
// DXGI (which follows the Windows HDR Calibration app's profile when there is
// one). SMS_HDR_CONTRAST and SMS_HDR_SATURATION are percent (100 leaves them
// as they are); SMS_HDR_HIGHLIGHTS (0 to 100) is how far the brightest parts
// reach toward the peak, in stops: 0 keeps SDR white at the paper white, 50
// puts it halfway between the paper white and the peak, 100 at the peak. Anything that fails leaves the usual SDR window and
// says why in the log.
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
#include <d3d11.h>
#include <dxgi1_6.h>
#endif

#include "gl_funcs.h"

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
const GLenum WGL_ACCESS_WRITE_DISCARD = 0x0002;
PFNOPEN wglDXOpenDevice;
PFNCLOSE wglDXCloseDevice;
PFNREGISTER wglDXRegisterObject;
PFNUNREGISTER wglDXUnregisterObject;
PFNLOCK wglDXLockObjects, wglDXUnlockObjects;

// ColorProfileGetDisplayDefault (mscms, Windows 11): the display's HDR profile,
// which the Windows HDR Calibration app sets.
typedef HRESULT(WINAPI* PFNPROFILE)(int scope, LUID adapter, UINT32 source, int type, int subtype, LPWSTR* name);
const int CPT_ICC_ = 0, CPST_EXTENDED_DISPLAY_COLOR_MODE_ = 8;

bool s_active;
ID3D11Device* s_dev;
ID3D11DeviceContext* s_ctx;
IDXGISwapChain3* s_chain;
ID3D11Texture2D* s_shared;
HANDLE s_dxDevice, s_dxObject;
GLuint s_hdrTex, s_hdrFbo, s_sdrTex, s_sdrFbo;
int s_w, s_h;
bool s_tearing;
float s_paper = 200, s_peak = 1000, s_contrast = 1, s_saturation = 1, s_highlights = 0.4f;

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

void releaseTargets() {
    if (s_dxObject) wglDXUnregisterObject(s_dxDevice, s_dxObject);
    s_dxObject = nullptr;
    if (s_shared) s_shared->Release();
    s_shared = nullptr;
    if (s_hdrFbo) glDeleteFramebuffers(1, &s_hdrFbo);
    if (s_hdrTex) glDeleteTextures(1, &s_hdrTex);
    if (s_sdrFbo) glDeleteFramebuffers(1, &s_sdrFbo);
    if (s_sdrTex) glDeleteTextures(1, &s_sdrTex);
    s_hdrFbo = s_hdrTex = s_sdrFbo = s_sdrTex = 0;
    s_w = s_h = 0;
}

void stop(const char* why) {
    logmsg("HDR: %s; the game shows in SDR", why);
    releaseTargets();
    if (s_dxDevice) wglDXCloseDevice(s_dxDevice);
    s_dxDevice = nullptr;
    if (s_chain) s_chain->Release();
    if (s_ctx) s_ctx->Release();
    if (s_dev) s_dev->Release();
    s_chain = nullptr;
    s_ctx = nullptr;
    s_dev = nullptr;
    s_active = false;
}

// The swap chain's buffers, the shared scRGB texture GL draws into, and the
// SDR texture the post passes draw into, all w x h.
bool makeTargets(int w, int h) {
    releaseTargets();
    if (FAILED(s_chain->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(s_dev->CreateTexture2D(&desc, nullptr, &s_shared))) return false;
    glGenTextures(1, &s_hdrTex);
    s_dxObject = wglDXRegisterObject(s_dxDevice, s_shared, s_hdrTex, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD);
    if (!s_dxObject) return false;
    glGenFramebuffers(1, &s_hdrFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_hdrFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_hdrTex, 0);
    glGenTextures(1, &s_sdrTex);
    glBindTexture(GL_TEXTURE_2D, s_sdrTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_sdrFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_sdrFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_sdrTex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_w = w;
    s_h = h;
    return ok;
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
        logmsg("HDR: the graphics driver has no WGL_NV_DX_interop2; the game shows in SDR");
        releaseOutputs(list);
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(out->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &s_dev, nullptr, &s_ctx))) {
        releaseOutputs(list);
        stop("no Direct3D 11 device");
        return false;
    }
    releaseOutputs(list);
    s_dxDevice = wglDXOpenDevice(s_dev);
    if (!s_dxDevice) {
        stop("OpenGL and Direct3D are not on the same graphics card");
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
        stop("no DXGI factory");
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
    const HRESULT made = factory->CreateSwapChainForHwnd(s_dev, hwnd, &desc, nullptr, nullptr, &chain);
    if (SUCCEEDED(made)) factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    factory->Release();
    if (FAILED(made) || FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s_chain)))) {
        if (chain) chain->Release();
        stop("no Direct3D swap chain for the window");
        return false;
    }
    chain->Release();
    if (FAILED(s_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
        stop("the swap chain cannot take scRGB");
        return false;
    }
    s_active = true;
    logmsg("HDR on: scRGB through Direct3D 11, paper white %.0f nits, peak %.0f nits, contrast %.0f%%, saturation %.0f%%, "
           "highlights %.0f%% (SDR white at %.0f nits)%s",
           double(s_paper), double(s_peak), double(s_contrast * 100), double(s_saturation * 100), double(s_highlights * 100),
           double(s_paper * powf(s_peak / s_paper, s_highlights)),
           config.profile.empty() ? "" : (", calibration profile " + config.profile).c_str());
    return true;
}

bool hdrFrameBegin(int w, int h) {
    if (!s_active || w <= 0 || h <= 0) return false;
    if ((w != s_w || h != s_h) && !makeTargets(w, h)) {
        stop("the swap chain could not be resized");
        return false;
    }
    g_presentFbo = s_sdrFbo;
    return true;
}

void hdrFramePresent(int vsync) {
    g_presentFbo = 0;
    if (!s_active) return;
    if (!wglDXLockObjects(s_dxDevice, 1, &s_dxObject)) {
        stop("the shared texture could not be locked");
        return;
    }
    hdrPass(s_sdrTex, s_w, s_h, s_hdrFbo, s_paper, s_peak, s_contrast, s_saturation, s_highlights);
    wglDXUnlockObjects(s_dxDevice, 1, &s_dxObject);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(s_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        s_ctx->CopyResource(back, s_shared);
        back->Release();
    }
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
