// Pre-game launcher: a settings menu shown in its own window before the game
// starts (GXPC_RunLauncher). It edits settings.txt and bindings.txt in place,
// keeping their comments, so everything it sets can also be set by hand; the
// game then reads both files as it always has.
//
// Dear ImGui (third_party/imgui) draws it through SDL2 and OpenGL 3.3, with
// its own GL loader: the game's renderer is not involved.
#include "sms_gx/gx_pc.h"

#ifdef SMS_GX_HAVE_SDL2
#include <SDL.h>
#include <SDL_opengl.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"  // ShadeVertsLinearColorGradientKeepAlpha
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

// PNG decoding for the background picture (its own static copy; gx_hires.cpp has another)
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third_party/stb_image.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <direct.h>
#else
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

// platform/netplay (weak: the launcher also links without it)
extern "C" __attribute__((weak)) int port_net_local_addresses(char* out, int size);
// platform/thp/hd_install.cpp: the HD cutscene catalog and patcher
#include "../../thp/hd_install.h"
extern "C" {
__attribute__((weak)) int port_hd_catalog(PortHdMovie* out, int max, char* release, int releaseSize);
__attribute__((weak)) const char* port_hd_catalog_json(void);
__attribute__((weak)) int port_hd_sha256_file(const char* path, char hex[65]);
__attribute__((weak)) int port_hd_apply(const char* disc, const PortHdMovie* movie, const char* patchPath,
                                         const char* outPath, char* err, int errSize);
}

namespace {

// ------------------------------------------------------------------ files
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool readLines(const std::string& path, std::vector<std::string>& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[2048];
    while (fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        out.push_back(l);
    }
    fclose(f);
    return true;
}

bool writeLines(const std::string& path, const std::vector<std::string>& lines) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

std::vector<std::string> listDirs(const std::string& dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && isDir(dir + "/" + e->d_name)) out.push_back(e->d_name);
        closedir(d);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

// settings.txt: `name = value` lines; a commented `# name = value` line is
// where a setting is written back when it was not set before.
struct SettingsFile {
    std::string path;
    std::vector<std::string> lines;
    std::map<std::string, std::string> values;

    static bool parse(const std::string& line, bool allowComment, std::string& key, std::string& val) {
        std::string s = trim(line);
        if (allowComment && !s.empty() && s[0] == '#') s = trim(s.substr(1));
        else if (s.empty() || s[0] == '#') return false;
        size_t eq = s.find('=');
        if (eq == std::string::npos) return false;
        key = trim(s.substr(0, eq));
        if (key.empty() || key.find(' ') != std::string::npos) return false;
        val = trim(s.substr(eq + 1));
        size_t c = val.find(" #");
        if (c != std::string::npos) val = trim(val.substr(0, c));
        return true;
    }
    void load() {
        values.clear();
        readLines(path, lines);
        for (const std::string& l : lines) {
            std::string k, v;
            if (parse(l, false, k, v)) values[k] = v;
        }
    }
    std::string get(const char* key, const char* def) const {
        auto it = values.find(key);
        return it == values.end() || it->second.empty() ? def : it->second;
    }
    void set(const char* key, const std::string& v) { values[key] = v; }
    bool save() {
        std::map<std::string, bool> done;
        for (std::string& l : lines) {  // active lines first
            std::string k, v;
            if (parse(l, false, k, v) && values.count(k) && !done[k]) {
                l = k + " = " + values[k];
                done[k] = true;
            }
        }
        for (std::string& l : lines) {  // then the commented examples
            std::string k, v;
            if (parse(l, true, k, v) && values.count(k) && !done[k] && trim(l)[0] == '#') {
                l = k + " = " + values[k];
                done[k] = true;
            }
        }
        bool header = false;
        for (auto& kv : values)
            if (!done[kv.first]) {
                if (!header) {
                    lines.push_back("");
                    lines.push_back("# Set from the launcher");
                    header = true;
                }
                lines.push_back(kv.first + " = " + kv.second);
            }
        return writeLines(path, lines);
    }
};

// ------------------------------------------------------------------ key bindings
const char* const kControls[] = {
    "A", "B", "X", "Y", "Z", "L", "R", "START",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
    "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT",
    "CSTICK_UP", "CSTICK_DOWN", "CSTICK_LEFT", "CSTICK_RIGHT",
    "HALF_TILT", "QUIT",
};
const char* const kControlLabels[] = {
    "A button", "B button", "X button", "Y button", "Z button", "L trigger", "R trigger", "Start",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Stick up", "Stick down", "Stick left", "Stick right",
    "C-stick up", "C-stick down", "C-stick left", "C-stick right",
    "Half tilt (walk)", "Quit game",
};
const int kNumControls = int(sizeof kControls / sizeof kControls[0]);
const char* const kDefaultKeys[] = {
    "SPACE X", "LSHIFT RSHIFT C", "V", "F", "Z", "Q", "E", "ENTER",
    "1 KP_8", "2 KP_2", "3 KP_4", "4 KP_6",
    "UP W", "DOWN S", "LEFT A", "RIGHT D",
    "I", "K", "J", "L",
    "LCTRL", "ESCAPE",
};
// the names platform/pad understands, by SDL scancode
struct KeyName { const char* name; int code; };
const KeyName kKeyNames[] = {
    {"A", 4}, {"B", 5}, {"C", 6}, {"D", 7}, {"E", 8}, {"F", 9}, {"G", 10}, {"H", 11}, {"I", 12}, {"J", 13},
    {"K", 14}, {"L", 15}, {"M", 16}, {"N", 17}, {"O", 18}, {"P", 19}, {"Q", 20}, {"R", 21}, {"S", 22},
    {"T", 23}, {"U", 24}, {"V", 25}, {"W", 26}, {"X", 27}, {"Y", 28}, {"Z", 29}, {"1", 30}, {"2", 31},
    {"3", 32}, {"4", 33}, {"5", 34}, {"6", 35}, {"7", 36}, {"8", 37}, {"9", 38}, {"0", 39}, {"ENTER", 40},
    {"ESCAPE", 41}, {"BACKSPACE", 42}, {"TAB", 43}, {"SPACE", 44}, {"MINUS", 45}, {"EQUALS", 46},
    {"LBRACKET", 47}, {"RBRACKET", 48}, {"SEMICOLON", 51}, {"APOSTROPHE", 52}, {"COMMA", 54},
    {"PERIOD", 55}, {"SLASH", 56}, {"F1", 58}, {"F2", 59}, {"F3", 60}, {"F4", 61}, {"RIGHT", 79},
    {"LEFT", 80}, {"DOWN", 81}, {"UP", 82}, {"KP_DIVIDE", 84}, {"KP_MULTIPLY", 85}, {"KP_MINUS", 86},
    {"KP_PLUS", 87}, {"KP_ENTER", 88}, {"KP_1", 89}, {"KP_2", 90}, {"KP_3", 91}, {"KP_4", 92},
    {"KP_5", 93}, {"KP_6", 94}, {"KP_7", 95}, {"KP_8", 96}, {"KP_9", 97}, {"KP_0", 98}, {"LCTRL", 224},
    {"LSHIFT", 225}, {"LALT", 226}, {"RCTRL", 228}, {"RSHIFT", 229}, {"RALT", 230},
};
std::string keyName(int scancode) {
    for (const KeyName& k : kKeyNames)
        if (k.code == scancode) return k.name;
    return "#" + std::to_string(scancode);
}
std::string prettyKey(const std::string& name) {
    if (!name.empty() && name[0] == '#') {
        const char* n = SDL_GetScancodeName(SDL_Scancode(atoi(name.c_str() + 1)));
        return n && *n ? n : name;
    }
    if (name == "ESCAPE") return "Esc";
    if (name == "LSHIFT") return "L-Shift";
    if (name == "RSHIFT") return "R-Shift";
    if (name == "LCTRL") return "L-Ctrl";
    if (name == "RCTRL") return "R-Ctrl";
    if (name == "LALT") return "L-Alt";
    if (name == "RALT") return "R-Alt";
    if (name.compare(0, 3, "KP_") == 0) return "Num " + name.substr(3);
    std::string s = name;
    for (size_t i = 1; i < s.size(); i++) s[i] = char(tolower(s[i]));
    return s;
}

struct BindingsFile {
    std::string path;
    std::vector<std::string> lines;
    std::string keys[kNumControls];

    void load() {
        for (int i = 0; i < kNumControls; i++) keys[i] = kDefaultKeys[i];
        readLines(path, lines);
        for (const std::string& l : lines) {
            std::string k, v;
            if (!SettingsFile::parse(l, false, k, v)) continue;
            for (int i = 0; i < kNumControls; i++)
                if (strcasecmp(k.c_str(), kControls[i]) == 0) keys[i] = v;
        }
    }
    // Updates the controls' lines in place (comments and order kept) and adds
    // a line for each changed control the file did not have.
    bool save() {
        std::vector<std::string> out = lines;
        bool present[kNumControls] = {};
        for (std::string& l : out) {
            std::string k, v;
            if (!SettingsFile::parse(l, false, k, v)) continue;
            for (int i = 0; i < kNumControls; i++)
                if (strcasecmp(k.c_str(), kControls[i]) == 0) {
                    if (v != keys[i]) l = std::string(kControls[i]) + " = " + keys[i];
                    present[i] = true;
                }
        }
        if (out.empty()) {
            out.push_back("# SMS port key bindings for controller 1 (written by the launcher).");
            out.push_back("# Format: CONTROL = KEY [KEY ...]; a line replaces that control's defaults.");
        }
        for (int i = 0; i < kNumControls; i++)
            if (!present[i] && keys[i] != kDefaultKeys[i]) out.push_back(std::string(kControls[i]) + " = " + keys[i]);
        if (out == lines) return true;  // unchanged: leave the file alone
        return writeLines(path, out);
    }
};

// ------------------------------------------------------------------ theme
const ImU32 kSky1 = IM_COL32(30, 136, 229, 255), kSky2 = IM_COL32(126, 211, 255, 255);
const ImU32 kSea1 = IM_COL32(0, 172, 193, 255), kSea2 = IM_COL32(0, 96, 160, 255);
const ImU32 kSun = IM_COL32(255, 214, 64, 255), kSunGlow = IM_COL32(255, 214, 64, 60);
const ImVec4 kAccent = ImVec4(1.00f, 0.79f, 0.24f, 1.0f);     // sunshine yellow
const ImVec4 kAccentHot = ImVec4(1.00f, 0.62f, 0.20f, 1.0f);  // orange
const ImVec4 kDim = ImVec4(0.66f, 0.76f, 0.91f, 1.0f);

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 14.0f;
    s.FrameRounding = 9.0f;
    s.GrabRounding = 9.0f;
    s.PopupRounding = 10.0f;
    s.ScrollbarRounding = 9.0f;
    s.TabRounding = 9.0f;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.WindowPadding = ImVec2(0, 0);
    s.ScrollbarSize = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.GrabMinSize = 18.0f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.10f, 0.20f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.17f, 0.32f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.19f, 0.36f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.25f, 0.45f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.32f, 0.56f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.37f, 0.63f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.27f, 0.49f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.37f, 0.64f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.43f, 0.72f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.12f, 0.27f, 0.49f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.37f, 0.64f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.43f, 0.72f, 1.0f);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHot;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.36f, 0.58f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_NavCursor] = kAccent;
    s.ScaleAllSizes(scale);
}

// ------------------------------------------------------------------ the menu
enum Page { P_INSTALL, P_DISPLAY, P_GRAPHICS, P_CAMERA, P_GAMEPLAY, P_ONLINE, P_AUDIO, P_CONTROLS, P_ABOUT, P_COUNT };
const char* const kPageNames[P_COUNT] = {"Install", "Display", "Graphics", "Camera", "Gameplay",
                                         "Online", "Audio", "Controls", "About"};
const char* const kPageBlurbs[P_COUNT] = {
    "Point the launcher at your own Super Mario Sunshine disc image to install the game.",
    "Window, monitor and how the picture fits your screen.",
    "Resolution, anti-aliasing, filtering and HD textures.",
    "Camera direction, speed, free camera and mouse look.",
    "Frame rate, movies, mods and the performance overlay.",
    "Play together: host a game or join a friend's, and see each other in the same level.",
    "Sound output and volume.",
    "Keyboard bindings for controller 1. Game controllers work automatically.",
    "About this build.",
};

struct Option { const char* value; const char* label; };

// ------------------------------------------------------------------ installer
// What a disc image is, from its header (plain/NKit ISO or GCM, or a Dolphin
// CISO, whose first block starts 0x8000 bytes in).
struct DiscCheck {
    bool ok = false;
    std::string summary;  // a line for the player: what it is, or what is wrong
};

FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    wchar_t wpath[1024], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 1024)) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    return _wfopen(wpath, wmode);
#else
    return fopen(path.c_str(), mode);
#endif
}

// A file's size, or -1. Read from the directory entry, never by opening the
// file: an open handle would make Windows refuse a rename of a download that
// is finishing while the progress bar asks for its size.
long long fileSize(const std::string& path) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, 1024)) return -1;
    const std::filesystem::path p(w);
#else
    const std::filesystem::path p(path);
#endif
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? -1 : (long long)n;
}

DiscCheck checkDisc(const std::string& path) {
    DiscCheck r;
    FILE* f = openUtf8(path, "rb");
    if (!f) {
        r.summary = "Cannot open this file.";
        return r;
    }
    unsigned char h[0x440] = {};
    size_t n = fread(h, 1, sizeof h, f);
    std::string kind = "ISO image";
    if (n >= 4 && !memcmp(h, "CISO", 4)) {
        kind = "Dolphin CISO image";
#ifdef _WIN32
        _fseeki64(f, 0x8000, SEEK_SET);
#else
        fseeko(f, 0x8000, SEEK_SET);
#endif
        n = fread(h, 1, sizeof h, f);
    }
    fclose(f);
    if (n >= 4 && (!memcmp(h, "RVZ\x01", 4) || !memcmp(h, "WIA\x01", 4))) {
        r.summary = "RVZ and WIA images are compressed in a way the port cannot read. In Dolphin, right-click the "
                    "game, choose Convert File and pick ISO, then select the new file.";
        return r;
    }
    const unsigned magic = unsigned(h[0x1C]) << 24 | unsigned(h[0x1D]) << 16 | unsigned(h[0x1E]) << 8 | h[0x1F];
    if (n < 0x440 || magic != 0xC2339F3Du) {
        r.summary = "This is not a GameCube disc image.";
        return r;
    }
    if (!memcmp(h + 0x200, "NKIT", 4)) kind = "NKit image";
    const std::string id(reinterpret_cast<const char*>(h), 6);
    if (id.compare(0, 3, "GMS") != 0) {
        r.summary = "This is a different GameCube game (" + id + "), not Super Mario Sunshine.";
        return r;
    }
    if (id != "GMSE01") {
        const char* region = id[3] == 'P' ? "European" : id[3] == 'J' ? "Japanese" : "other";
        r.summary = std::string("This is the ") + region + " release (" + id +
                    "). The port needs the North American release, GMSE01.";
        return r;
    }
    if (h[7] != 0) {
        r.summary = "This is revision " + std::to_string(h[7]) + ". The port needs revision 0 of GMSE01.";
        return r;
    }
    char size[32];
    snprintf(size, sizeof size, "%.2f GB", double(fileSize(path)) / 1e9);
    r.ok = true;
    r.summary = "Super Mario Sunshine, North America (GMSE01, revision 0), " + kind + ", " + size + ".";
    return r;
}

// Asks the desktop for a file: the Windows file dialog, zenity or kdialog on
// Linux, AppleScript on macOS. Empty when cancelled or unavailable.
std::string browseForImage() {
#ifdef _WIN32
    typedef BOOL(WINAPI * GetOpenFileNameWFn)(LPOPENFILENAMEW);
    static HMODULE dlg = LoadLibraryA("comdlg32.dll");
    GetOpenFileNameWFn open = dlg ? (GetOpenFileNameWFn)GetProcAddress(dlg, "GetOpenFileNameW") : nullptr;
    if (!open) return std::string();
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"GameCube disc images (*.iso, *.gcm, *.ciso)\0*.iso;*.gcm;*.ciso\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Select your Super Mario Sunshine disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!open(&ofn)) return std::string();
    char utf8[2048];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof utf8, nullptr, nullptr);
    return utf8;
#else
    const char* const cmds[] = {
#ifdef __APPLE__
        "osascript -e 'POSIX path of (choose file with prompt \"Select your Super Mario Sunshine disc image\")' "
        "2>/dev/null",
#else
        "zenity --file-selection --title='Select your Super Mario Sunshine disc image' "
        "--file-filter='GameCube disc images | *.iso *.gcm *.ciso *.ISO *.GCM' --file-filter='All files | *' "
        "2>/dev/null",
        "kdialog --getopenfilename . '*.iso *.gcm *.ciso' 2>/dev/null",
#endif
    };
    for (const char* cmd : cmds) {
        FILE* p = popen(cmd, "r");
        if (!p) continue;
        char buf[4096] = "";
        const bool got = fgets(buf, sizeof buf, p) != nullptr;
        const int rc = pclose(p);
        std::string s = trim(buf);
        if (got && rc == 0 && !s.empty()) return s;
        if (rc == 0 || rc == 256) return std::string();  // ran, and the player cancelled
    }
    return std::string();
#endif
}

// Copies the image into rom/ on a worker thread.
struct InstallJob {
    std::thread worker;
    std::atomic<bool> running{false}, cancel{false}, done{false}, failed{false};
    std::atomic<long long> copied{0};
    long long total = 0;
    std::string dest, error;

    void start(const std::string& src, const std::string& to) {
        if (worker.joinable()) worker.join();
        dest = to;
        error.clear();
        total = fileSize(src);
        copied = 0;
        cancel = done = failed = false;
        running = true;
        worker = std::thread([this, src]() { run(src); });
    }
    void run(const std::string src) {
        const std::string part = dest + ".part";
        FILE* in = openUtf8(src, "rb");
        FILE* out = in ? openUtf8(part, "wb") : nullptr;
        if (!in || !out) {
            error = in ? "Cannot write to " + part : "Cannot read " + src;
            if (in) fclose(in);
            failed = true;
            running = false;
            return;
        }
        std::vector<char> buf(8 << 20);
        size_t n;
        while (!cancel && (n = fread(buf.data(), 1, buf.size(), in)) > 0) {
            if (fwrite(buf.data(), 1, n, out) != n) {
                error = "Writing failed: is the disk full?";
                break;
            }
            copied += (long long)n;
        }
        fclose(in);
        const bool ok = fclose(out) == 0 && error.empty() && !cancel;
        if (ok) {
            remove(dest.c_str());
            if (rename(part.c_str(), dest.c_str()) != 0) error = "Cannot rename " + part;
        }
        if (!ok || !error.empty()) {
            remove(part.c_str());
            if (error.empty() && !cancel) error = "Copy failed.";
            failed = !cancel;
        } else {
            done = true;
        }
        running = false;
    }
    ~InstallJob() {
        cancel = true;
        if (worker.joinable()) worker.join();
    }
};

// ------------------------------------------------------------------ child processes
// A helper program (curl, tar) run without a console window, its stdout and
// stderr read line by line; kill() stops it.
struct Process {
#ifdef _WIN32
    HANDLE proc = nullptr, out = nullptr;
#else
    pid_t pid = -1;
    int out = -1;
#endif
    std::string pending;

    static std::string quote(const std::string& a) {
#ifdef _WIN32
        std::string q = "\"";
        for (char c : a) q += c == '"' ? std::string("\\\"") : std::string(1, c);
        return q + "\"";
#else
        return a;
#endif
    }
    bool start(const std::vector<std::string>& args) {
#ifdef _WIN32
        std::string cmd;
        for (const std::string& a : args) cmd += (cmd.empty() ? "" : " ") + quote(a);
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi;
        std::vector<wchar_t> wcmd(cmd.size() * 2 + 16);
        MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), int(wcmd.size()));
        const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                       &si, &pi);
        CloseHandle(wr);
        if (!ok) {
            CloseHandle(rd);
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        out = rd;
        return true;
#else
        int fds[2];
        if (pipe(fds) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fds[1]);
        if (rc != 0) {
            close(fds[0]);
            pid = -1;
            return false;
        }
        out = fds[0];
        return true;
#endif
    }
    // the next line of output; false at the end
    bool line(std::string& l) {
        for (;;) {
            const size_t nl = pending.find_first_of("\r\n");
            if (nl != std::string::npos) {
                l = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                return true;
            }
            char buf[4096];
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(out, buf, sizeof buf, &n, nullptr) || n == 0) break;
#else
            const ssize_t n = read(out, buf, sizeof buf);
            if (n <= 0) break;
#endif
            pending.append(buf, size_t(n));
        }
        if (pending.empty()) return false;
        l.swap(pending);
        pending.clear();
        return true;
    }
    int wait() {  // exit code
#ifdef _WIN32
        if (!proc) return -1;
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        CloseHandle(out);
        proc = out = nullptr;
        return int(code);
#else
        if (pid < 0) return -1;
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        close(out);
        pid = -1;
        out = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }
    void kill() {
#ifdef _WIN32
        if (proc) TerminateProcess(proc, 1);
#else
        if (pid > 0) ::kill(pid, SIGTERM);
#endif
    }
};

bool runs(const std::vector<std::string>& args) {  // the program starts and exits 0
    Process p;
    if (!p.start(args)) return false;
    std::string l;
    while (p.line(l)) {
    }
    return p.wait() == 0;
}

// ------------------------------------------------------------------ HD texture pack
// The Super Mario Sunshine UHD Texture Pack (qashto, razius), pinned to the
// release tools/mods/get.py installs, downloaded from its own GitHub release
// and unpacked into mods/textures/GMS: download with curl (resuming a partial
// file), unpack with a tar that reads 7z (bundled in the release packages,
// else the system's), or 7-Zip.
const char* const kPackUrl =
    "https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack/releases/download/2.1.1/GMS.7z";
const long long kPackSize = 986431331LL;      // bytes, GMS.7z
const long long kPackUnpacked = 3218563135LL;  // bytes, installed
const int kPackEntries = 2237;                 // archive entries under GMS/Textures/GMS

struct TexturePackJob {
    enum Phase { IDLE, DOWNLOADING, EXTRACTING, REMOVING, DONE, FAILED, CANCELLED };
    std::thread worker;
    std::atomic<int> phase{IDLE};
    std::atomic<bool> cancel{false};
    std::atomic<int> entries{0};
    std::mutex mu;
    Process* child = nullptr;  // under mu
    std::string error;
    std::string modsDir, exeDir;

    bool busy() const { return phase == DOWNLOADING || phase == EXTRACTING || phase == REMOVING; }
    std::string archive() const { return modsDir + ".downloads/GMS.7z"; }
    std::string installDir() const { return modsDir + "textures/GMS"; }
    float progress() const {
        if (phase == DOWNLOADING) {
            const long long n = std::max(fileSize(archive() + ".part"), fileSize(archive()));
            return float(double(std::max(0LL, n)) / double(kPackSize));
        }
        if (phase == EXTRACTING) return std::min(1.0f, float(entries) / float(kPackEntries));
        return 0.0f;
    }

    void begin(void (TexturePackJob::*fn)()) {
        if (worker.joinable()) worker.join();
        cancel = false;
        error.clear();
        entries = 0;
        worker = std::thread(fn, this);
    }
    void stop() {
        cancel = true;
        std::lock_guard<std::mutex> lk(mu);
        if (child) child->kill();
    }
    ~TexturePackJob() {
        stop();
        if (worker.joinable()) worker.join();
    }

    int run(Process& p, const std::vector<std::string>& args, std::atomic<int>* lines) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancel || !p.start(args)) return -1;
            child = &p;
        }
        std::string l, last;
        while (p.line(l))
            if (!l.empty()) {
                last = l;
                if (lines) ++*lines;
            }
        const int rc = p.wait();
        std::lock_guard<std::mutex> lk(mu);
        child = nullptr;
        if (rc != 0 && error.empty()) error = last;
        return rc;
    }
    void fail(const std::string& why) {
        if (cancel) {
            phase = CANCELLED;
            return;
        }
        if (error.empty() || why.find(':') == std::string::npos) error = why + (error.empty() ? "" : ": " + error);
        phase = FAILED;
    }

    // the first extractor found: {program, true for bsdtar syntax / false for 7-Zip}
    std::pair<std::string, bool> extractor() {
        std::vector<std::pair<std::string, bool>> c;
#ifdef _WIN32
        c.push_back({exeDir + "tools\\bsdtar.exe", true});
        char sys[MAX_PATH];
        if (GetSystemDirectoryA(sys, MAX_PATH)) c.push_back({std::string(sys) + "\\tar.exe", true});
        c.push_back({"C:\\Program Files\\7-Zip\\7z.exe", false});
        c.push_back({"7z", false});
        for (auto& e : c)
            if (e.first.find('\\') == std::string::npos ? runs({e.first}) : fileSize(e.first) > 0) {
                if (e.second && !runs({e.first, "--version"})) continue;
                return e;
            }
#else
        c.push_back({exeDir + "bsdtar", true});
        c.push_back({"bsdtar", true});
        c.push_back({"7zz", false});
        c.push_back({"7z", false});
        c.push_back({"7za", false});
        for (auto& e : c)
            if (runs({e.first, e.second ? "--version" : "i"})) return e;
#endif
        return {std::string(), false};
    }

    void install() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(modsDir + ".downloads", ec);
        fs::create_directories(modsDir + "textures", ec);
        const long long have = std::max(0LL, fileSize(archive() + ".part")) + std::max(0LL, fileSize(archive()));
        const auto space = fs::space(modsDir + "textures", ec);
        if (!ec && (long long)space.available < kPackUnpacked + kPackSize - have + (256LL << 20)) {
            char buf[160];
            snprintf(buf, sizeof buf, "Not enough free space: about %.1f GB is needed",
                     double(kPackUnpacked + kPackSize - have) / 1e9);
            fail(buf);
            return;
        }
        // download (resuming a partial one)
        if (fileSize(archive()) != kPackSize) {
            phase = DOWNLOADING;
            Process p;
#ifdef _WIN32
            char sys[MAX_PATH] = "";
            GetSystemDirectoryA(sys, MAX_PATH);
            const std::string curl = fileSize(std::string(sys) + "\\curl.exe") > 0 ? std::string(sys) + "\\curl.exe"
                                                                                   : std::string("curl");
#else
            const std::string curl = "curl";
#endif
            int rc = run(p, {curl, "-L", "-f", "-sS", "--retry", "3", "-C", "-", "-o", archive() + ".part", kPackUrl},
                         nullptr);
#ifndef _WIN32
            if (rc < 0 && !cancel) {  // no curl: wget
                Process w;
                error.clear();
                rc = run(w, {"wget", "-q", "-c", "-O", archive() + ".part", kPackUrl}, nullptr);
            }
#endif
            if (cancel) return fail("");
            if (rc != 0 || fileSize(archive() + ".part") != kPackSize) {
                if (fileSize(archive() + ".part") > kPackSize) fs::remove(archive() + ".part", ec);
                return fail(rc < 0 ? "Cannot run curl to download the pack" : "The download failed");
            }
            fs::rename(archive() + ".part", archive(), ec);
            if (ec) return fail("Cannot rename the download: " + ec.message());
        }
        // unpack into a staging folder, then move it into place
        phase = EXTRACTING;
        const auto tool = extractor();
        if (tool.first.empty())
            return fail("No program to unpack .7z archives was found. Install 7-Zip (Windows) or "
                        "libarchive-tools / p7zip (Linux), or extract GMS/Textures/GMS from GMS.7z into "
                        "mods/textures/ yourself");
        const std::string stage = modsDir + ".downloads/stage";
        fs::remove_all(stage, ec);
        fs::create_directories(stage, ec);
        Process p;
        int rc;
        std::string unpacked;
        if (tool.second) {
            rc = run(p, {tool.first, "-xvf", archive(), "-C", stage, "--strip-components", "2", "GMS/Textures/GMS"},
                     &entries);
            unpacked = stage + "/GMS";
        } else {
            rc = run(p, {tool.first, "x", "-y", "-bb1", "-o" + stage, archive(), "GMS/Textures/GMS/*"}, &entries);
            unpacked = stage + "/GMS/Textures/GMS";
        }
        if (cancel || rc != 0 || !isDir(unpacked)) {
            fs::remove_all(stage, ec);
            return fail(rc < 0 ? "Cannot run " + tool.first : "Unpacking failed");
        }
        fs::remove_all(installDir(), ec);
        fs::rename(unpacked, installDir(), ec);
        if (ec) return fail("Cannot move the pack into place: " + ec.message());
        fs::remove_all(stage, ec);
        fs::remove(archive(), ec);
        phase = DONE;
    }
    void uninstall() {
        phase = REMOVING;
        std::error_code ec;
        std::filesystem::remove_all(installDir(), ec);
        if (ec) return fail("Cannot remove " + installDir() + ": " + ec.message());
        phase = DONE;
    }
};

std::string curlPath() {
#ifdef _WIN32
    char sys[MAX_PATH] = "";
    GetSystemDirectoryA(sys, MAX_PATH);
    return fileSize(std::string(sys) + "\\curl.exe") > 0 ? std::string(sys) + "\\curl.exe" : std::string("curl");
#else
    return "curl";
#endif
}

// ------------------------------------------------------------------ updates
// Checks this fork's latest GitHub release and installs it in place. Windows:
// the release zip is unpacked beside sms.exe; files in use (sms.exe, its DLLs)
// are renamed to *.old first, which Windows allows, and removed on the next
// start. Linux: the AppImage ($APPIMAGE) is replaced. settings.txt,
// bindings.txt, rom/ and mods/ are never touched.
#ifndef SMS_PORT_VERSION
#define SMS_PORT_VERSION "dev"
#endif
const char* const kPortVersion = SMS_PORT_VERSION;
const char* const kUpdateApi = "https://api.github.com/repos/TekRantGaming/sms-pc-port/releases/latest";
const char* const kReleasesPage = "https://github.com/TekRantGaming/sms-pc-port/releases/latest";
const char* const kSmsLauncherPage = "https://github.com/chasem-dev/sms-launcher";

// "v1.4.0-3-gabc" -> {1, 4, 0}; empty when it is not a version
std::vector<int> parseVersion(const std::string& s) {
    std::vector<int> v;
    size_t i = s[0] == 'v' || s[0] == 'V' ? 1 : 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
        int n = 0;
        while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
        v.push_back(n);
        if (i < s.size() && s[i] == '.') i++;
        else break;
    }
    return v;
}

// the string value of "key": in a JSON text, from `from`; npos when absent
size_t jsonString(const std::string& j, const char* key, std::string& out, size_t from = 0) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return p;
    p = j.find('"', j.find(':', p + k.size()));
    if (p == std::string::npos) return p;
    out.clear();
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] == '\\' && i + 1 < j.size()) i++;
        out += j[i];
    }
    return p;
}

struct UpdateJob {
    enum Phase { IDLE, CHECKING, LATEST, AVAILABLE, DOWNLOADING, INSTALLING, READY, FAILED, CANCELLED };
    std::thread worker;
    std::atomic<int> phase{IDLE};
    std::atomic<bool> cancel{false};
    std::mutex mu;
    Process* child = nullptr;  // under mu
    std::string error, latest, assetUrl, exeDir;
    long long assetSize = 0;

    bool busy() const { return phase == CHECKING || phase == DOWNLOADING || phase == INSTALLING; }
    std::string dir() const { return exeDir + ".update/"; }
    std::string download() const {
#ifdef _WIN32
        return dir() + "update.zip";
#else
        const char* a = getenv("APPIMAGE");
        return std::string(a ? a : "") + ".new";
#endif
    }
    float progress() const {
        return assetSize > 0 ? std::min(1.0f, float(double(std::max(0LL, fileSize(download() + ".part"))) /
                                                    double(assetSize)))
                             : 0.0f;
    }
    // installing replaces the program itself, which is only possible for the
    // release packages (not a build from source or the in-tree build folder)
    static bool canInstall(const std::string& exeDir) {
#ifdef _WIN32
        return fileSize(exeDir + "tools\\bsdtar.exe") > 0;
#else
        (void)exeDir;
        const char* a = getenv("APPIMAGE");
        return a && *a;
#endif
    }

    void begin(void (UpdateJob::*fn)()) {
        if (worker.joinable()) worker.join();
        cancel = false;
        error.clear();
        worker = std::thread(fn, this);
    }
    void stop() {
        cancel = true;
        std::lock_guard<std::mutex> lk(mu);
        if (child) child->kill();
    }
    ~UpdateJob() {
        stop();
        if (worker.joinable()) worker.join();
    }
    int run(const std::vector<std::string>& args, std::string* output) {
        Process p;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancel || !p.start(args)) return -1;
            child = &p;
        }
        std::string l, last;
        while (p.line(l)) {
            if (output) *output += l + "\n";
            if (!l.empty()) last = l;
        }
        const int rc = p.wait();
        std::lock_guard<std::mutex> lk(mu);
        child = nullptr;
        if (rc != 0 && error.empty()) error = last;
        return rc;
    }
    void fail(const std::string& why) {
        if (cancel) {
            phase = CANCELLED;
            return;
        }
        error = why + (error.empty() ? "" : ": " + error);
        phase = FAILED;
    }

    void check() {
        phase = CHECKING;
        std::string json;
        if (run({curlPath(), "-sS", "-f", "-L", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                 "-H", std::string("User-Agent: sms-pc-port/") + kPortVersion, kUpdateApi},
                &json) != 0)
            return fail("Cannot reach GitHub");
        if (jsonString(json, "tag_name", latest) == std::string::npos) return fail("No release was found");
#ifdef _WIN32
        const char* suffix = "-windows-x64.zip";
#else
        const char* suffix = "-x86_64.AppImage";
#endif
        assetUrl.clear();
        assetSize = 0;
        std::string url;
        for (size_t p = 0; (p = jsonString(json, "browser_download_url", url, p)) != std::string::npos; p++) {
            if (url.size() < strlen(suffix) || url.compare(url.size() - strlen(suffix), std::string::npos, suffix))
                continue;
            assetUrl = url;
            // "size" comes before "browser_download_url" in each asset
            const size_t s = json.rfind("\"size\"", p);
            if (s != std::string::npos) assetSize = atoll(json.c_str() + json.find(':', s) + 1);
            break;
        }
        const std::vector<int> have = parseVersion(kPortVersion), got = parseVersion(latest);
        // a build that is not a release (dev) never offers updates on its own
        phase = !have.empty() && got > have ? AVAILABLE : LATEST;
    }

    void install() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (assetUrl.empty()) return fail("This release has no download for this system");
        fs::create_directories(dir(), ec);
        phase = DOWNLOADING;
        const std::string part = download() + ".part";
        fs::remove(part, ec);
        if (run({curlPath(), "-L", "-f", "-sS", "--retry", "3", "-o", part, assetUrl}, nullptr) != 0 ||
            (assetSize > 0 && fileSize(part) != assetSize)) {
            fs::remove(part, ec);
            return fail("The download failed");
        }
        phase = INSTALLING;
#ifdef _WIN32
        const std::string stage = dir() + "stage";
        fs::remove_all(stage, ec);
        fs::create_directories(stage, ec);
        if (run({exeDir + "tools\\bsdtar.exe", "-xf", part, "-C", stage}, nullptr) != 0)
            return fail("Cannot unpack the update");
        fs::remove(part, ec);
        // the zip holds one folder, SMS-PC-Port-<version>-windows-x64
        fs::path root = stage;
        for (auto& e : fs::directory_iterator(stage, ec))
            if (e.is_directory() && fs::exists(e.path() / "sms.exe")) root = e.path();
        if (!fs::exists(root / "sms.exe")) return fail("The update has no sms.exe");
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            const fs::path rel = fs::relative(it->path(), root, ec);
            const fs::path to = fs::path(exeDir) / rel;
            if (it->is_directory()) {
                fs::create_directories(to, ec);
                continue;
            }
            const std::string name = rel.generic_string();
            if ((name == "settings.txt" || name == "bindings.txt") && fs::exists(to)) continue;  // the player's
            if (fs::exists(to)) {
                fs::path old = to;
                old += ".old";
                fs::remove(old, ec);
                for (int n = 1; fs::exists(old) && n < 100; n++) {  // an older one still in use
                    old = to;
                    old += "." + std::to_string(n) + ".old";
                    fs::remove(old, ec);
                }
                fs::rename(to, old, ec);
                if (ec) return fail("Cannot replace " + name + ": " + ec.message());
            }
            fs::rename(it->path(), to, ec);
            if (ec) return fail("Cannot install " + name + ": " + ec.message());
        }
        fs::remove_all(dir(), ec);
#else
        chmod(part.c_str(), 0755);
        fs::rename(part, getenv("APPIMAGE"), ec);
        if (ec) return fail("Cannot replace the AppImage: " + ec.message());
#endif
        phase = READY;
    }

    // removes what an earlier update left behind (*.old files in use then)
    static void cleanup(const std::string& exeDir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (exeDir.empty()) return;
        fs::remove_all(exeDir + ".update", ec);
        for (const std::string d : {exeDir, exeDir + "tools"})  // where the packages put files
            for (auto& e : fs::directory_iterator(d, ec)) {
                const std::string n = e.path().filename().string();
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".old") == 0) fs::remove(e.path(), ec);
            }
    }
};

// ------------------------------------------------------------------ the move to SMS Launcher
// This fork is closed: its features went to chasem-dev's port and SMS Launcher,
// which builds the game from the player's own disc and keeps it up to date.
// The launcher now only downloads SMS Launcher's latest release from GitHub
// and starts it (the Windows installer, or the Linux AppImage).
const char* const kSmsLauncherApi = "https://api.github.com/repos/chasem-dev/sms-launcher/releases/latest";
const char* const kSmsLauncherReleases = "https://github.com/chasem-dev/sms-launcher/releases/latest";

struct MoveJob {
    enum Phase { IDLE, SAVES, FINDING, DOWNLOADING, STARTING, DONE, FAILED, CANCELLED };
    std::thread worker;
    std::atomic<int> phase{IDLE};
    std::atomic<bool> cancel{false};
    std::mutex mu;
    Process* child = nullptr;  // under mu
    std::string error, version, assetName, assetUrl, file;
    long long assetSize = 0;
    // memory card: where this game kept it (set before begin), and what happened to it
    std::string cardFrom, cardTo, cardBackup, cardResult;

    bool busy() const { return phase == SAVES || phase == FINDING || phase == DOWNLOADING || phase == STARTING; }
    float progress() const {
        return assetSize > 0 ? std::min(1.0f, float(double(std::max(0LL, fileSize(file + ".part"))) / double(assetSize))) : 0.0f;
    }
    void begin() {
        if (worker.joinable()) worker.join();
        cancel = false;
        error.clear();
        worker = std::thread(&MoveJob::run, this);
    }
    void stop() {
        cancel = true;
        std::lock_guard<std::mutex> lk(mu);
        if (child) child->kill();
    }
    ~MoveJob() {
        stop();
        if (worker.joinable()) worker.join();
    }
    int exec(const std::vector<std::string>& args, std::string* output) {
        Process p;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancel || !p.start(args)) return -1;
            child = &p;
        }
        std::string l, last;
        while (p.line(l)) {
            if (output) *output += l + "\n";
            if (!l.empty()) last = l;
        }
        const int rc = p.wait();
        std::lock_guard<std::mutex> lk(mu);
        child = nullptr;
        if (rc != 0 && error.empty()) error = last;
        return rc;
    }
    void fail(const std::string& why) {
        if (cancel) {
            phase = CANCELLED;
            return;
        }
        error = why + (error.empty() ? "" : ": " + error);
        phase = FAILED;
    }
    // where the download goes: the user's Downloads folder, else the temporary one
    static std::string downloadDir() {
        namespace fs = std::filesystem;
        std::error_code ec;
#ifdef _WIN32
        const char* home = getenv("USERPROFILE");
#else
        const char* home = getenv("HOME");
#endif
        if (home && *home && fs::is_directory(fs::path(home) / "Downloads", ec))
            return (fs::path(home) / "Downloads").string();
        return fs::temp_directory_path(ec).string();
    }

    // SMS Launcher's memory card folder (its saves.js saveDirectory, with no save_dir of its own), the
    // same default this port uses: $XDG_DATA_HOME, %APPDATA% on Windows, else ~/.local/share.
    static std::string launcherCardDir() {
        namespace fs = std::filesystem;
        if (const char* x = getenv("XDG_DATA_HOME")) return (fs::path(x) / "sms-port" / "card-a").string();
#ifdef _WIN32
        if (const char* x = getenv("APPDATA")) return (fs::path(x) / "sms-port" / "card-a").string();
#endif
        const char* home = getenv("HOME");
        return (fs::path(home ? home : ".") / ".local" / "share" / "sms-port" / "card-a").string();
    }
    static bool isCardFile(const std::string& name) {
        auto ends = [&](const char* s) { return name.size() > strlen(s) && name.compare(name.size() - strlen(s), std::string::npos, s) == 0; };
        return name == "index.txt" || ends(".dat") || ends(".stat");
    }
    static std::vector<std::string> cardFiles(const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<std::string> out;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && isCardFile(e.path().filename().string())) out.push_back(e.path().filename().string());
        return out;
    }

    // Backs the memory card up beside the player's other files, then copies it into SMS Launcher's
    // card folder when it was kept elsewhere (save_dir). A card already there is never overwritten.
    bool migrateSaves() {
        namespace fs = std::filesystem;
        std::error_code ec;
        cardTo = launcherCardDir();
        const std::vector<std::string> files = cardFiles(cardFrom);
        if (files.empty()) {
            cardResult = "No saves were found to move.";
            return true;
        }
        // a dated backup, every time
#ifdef _WIN32
        const char* home = getenv("USERPROFILE");
#else
        const char* home = getenv("HOME");
#endif
        char stamp[32];
        const time_t now = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", localtime(&now));
        cardBackup = (fs::path(home && *home ? home : ".") / (std::string("SMS PC Port save backup ") + stamp)).string();
        fs::create_directories(cardBackup, ec);
        for (const std::string& f : files) {
            fs::copy_file(fs::path(cardFrom) / f, fs::path(cardBackup) / f, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
        }
        if (fs::equivalent(cardFrom, cardTo, ec)) {
            cardResult = "Your saves are already in SMS Launcher's memory card folder. A backup copy is in " + cardBackup + ".";
            return true;
        }
        if (!cardFiles(cardTo).empty()) {
            cardResult = "SMS Launcher's memory card already has saves, so it was left as it is. Your saves from this "
                         "version are backed up in " + cardBackup + ".";
            return true;
        }
        fs::create_directories(cardTo, ec);
        for (const std::string& f : files) {
            fs::copy_file(fs::path(cardFrom) / f, fs::path(cardTo) / f, fs::copy_options::skip_existing, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
        }
        cardResult = "Your saves were copied to SMS Launcher's memory card folder (" + cardTo + "). A backup copy is in " +
                     cardBackup + ".";
        return true;
    }

    void run() {
        namespace fs = std::filesystem;
        std::error_code ec;
        phase = SAVES;
        if (!migrateSaves()) return fail("Your saves could not be backed up, so nothing was changed");
        phase = FINDING;
        std::string json;
        if (exec({curlPath(), "-sS", "-f", "-L", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
                  "-H", std::string("User-Agent: sms-pc-port/") + kPortVersion, kSmsLauncherApi},
                 &json) != 0)
            return fail("Cannot reach GitHub");
        jsonString(json, "tag_name", version);
        std::string url;
        for (size_t p = 0; (p = jsonString(json, "browser_download_url", url, p)) != std::string::npos; p++) {
            const std::string name = url.substr(url.find_last_of('/') + 1);
#ifdef _WIN32
            const bool want = name.rfind("SMS-Launcher-Setup-", 0) == 0 && name.size() > 4 &&
                              name.compare(name.size() - 4, 4, ".exe") == 0;
#else
            const bool want = name.size() > 9 && name.compare(name.size() - 9, 9, ".AppImage") == 0;
#endif
            if (!want) continue;
            assetUrl = url;
            assetName = name;
            const size_t s = json.rfind("\"size\"", p);
            if (s != std::string::npos) assetSize = atoll(json.c_str() + json.find(':', s) + 1);
            break;
        }
        if (assetUrl.empty()) return fail("SMS Launcher's latest release has no download for this system");
        file = (fs::path(downloadDir()) / assetName).string();
        if (fileSize(file) != assetSize || assetSize <= 0) {
            phase = DOWNLOADING;
            const std::string part = file + ".part";
            if (exec({curlPath(), "-L", "-f", "-sS", "--retry", "3", "-C", "-", "-o", part, assetUrl}, nullptr) != 0 ||
                (assetSize > 0 && fileSize(part) != assetSize)) {
                if (fileSize(part) > assetSize) fs::remove(part, ec);
                return fail("The download failed");
            }
            fs::remove(file, ec);
            fs::rename(part, file, ec);
            if (ec) return fail("Cannot save " + file + ": " + ec.message());
        }
        phase = STARTING;
#ifdef _WIN32
        // the NSIS installer, detached: it outlives this program
        std::wstring wfile(file.size() + 1, L'\0');
        wfile.resize(MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &wfile[0], int(wfile.size())) - 1);
        std::wstring cmd = L"\"" + wfile + L"\"";
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(wfile.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si,
                            &pi))
            return fail("Downloaded to " + file + ", but it could not be started; open it from there");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
#else
        chmod(file.c_str(), 0755);
        std::string self = file;
        char* args[] = {&self[0], nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, self.c_str(), nullptr, nullptr, args, environ) != 0)
            return fail("Downloaded to " + file + ", but it could not be started; run it from there");
#endif
        phase = DONE;
    }
};

// starts the (updated) game again, showing the launcher
bool restartSelf() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(NULL, exe, sizeof exe / sizeof exe[0])) return false;
    wchar_t cmd[MAX_PATH * 2 + 32];
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --launcher", exe);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const char* a = getenv("APPIMAGE");
    if (!a || !*a) return false;
    std::string self = a;
    char forced[] = "--launcher";
    char* args[] = {&self[0], forced, nullptr};
    pid_t pid = 0;
    return posix_spawn(&pid, self.c_str(), nullptr, nullptr, args, environ) == 0;
#endif
}

// ------------------------------------------------------------------ HD cutscenes
// The 3x AI-enhanced movies (docs/HD-CUTSCENES.md): for each of the 21, the
// SMP1 patch from the catalog's release is downloaded and checked, applied to
// the original movie read from the player's disc image, and checked again
// (platform/thp/hd_install.cpp). The pack is built in a staging folder and
// moved to mods/hd-cutscenes only once all 21 pass, so a cancelled or failed
// install keeps whatever was there before.
const char* const kHdMarker = "sms-hd-cutscenes-v1.complete";

struct HdCutsceneJob {
    enum Phase { IDLE, WORKING, REMOVING, DONE, FAILED, CANCELLED };
    std::thread worker;
    std::atomic<int> phase{IDLE};
    std::atomic<bool> cancel{false};
    std::atomic<int> movie{0}, step{0};  // 1..21; 0 downloading, 1 preparing
    std::atomic<long long> doneBytes{0};
    long long totalBytes = 1;
    std::mutex mu;
    Process* child = nullptr;
    std::string error, modsDir, disc, part;

    bool busy() const { return phase == WORKING || phase == REMOVING; }
    std::string packDir() const { return modsDir + "hd-cutscenes"; }
    float progress() {
        long long now = doneBytes;
        if (step == 0) {
            std::lock_guard<std::mutex> lk(mu);
            if (!part.empty()) now += std::max(0LL, fileSize(part));
        }
        return std::min(1.0f, float(double(now) / double(totalBytes)));
    }
    void begin(void (HdCutsceneJob::*fn)()) {
        if (worker.joinable()) worker.join();
        cancel = false;
        error.clear();
        movie = step = 0;
        doneBytes = 0;
        phase = fn == &HdCutsceneJob::uninstall ? REMOVING : WORKING;
        worker = std::thread(fn, this);
    }
    void stop() {
        cancel = true;
        std::lock_guard<std::mutex> lk(mu);
        if (child) child->kill();
    }
    ~HdCutsceneJob() {
        stop();
        if (worker.joinable()) worker.join();
    }
    void fail(const std::string& why) {
        if (cancel) {
            phase = CANCELLED;
            return;
        }
        error = why;
        phase = FAILED;
    }

    void install() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!port_hd_catalog || !port_hd_apply || !port_hd_sha256_file || !port_hd_catalog_json)
            return fail("This build cannot install HD cutscenes");
        static PortHdMovie movies[32];
        char release[64] = "";
        const int n = port_hd_catalog(movies, 32, release, sizeof release);
        if (n != 21) return fail("The HD cutscene catalog is incomplete");
        long long total = 0, targets = 0, biggestPatch = 0;
        for (int i = 0; i < n; i++) {
            total += movies[i].patch_bytes;
            targets += movies[i].target_bytes;
            biggestPatch = std::max(biggestPatch, movies[i].patch_bytes);
        }
        totalBytes = total;
        fs::create_directories(modsDir, ec);
        const auto space = fs::space(modsDir, ec);
        const long long needed = targets + (900LL << 20) + biggestPatch + (64LL << 20);
        if (!ec && (long long)space.available < needed) {
            char buf[128];
            snprintf(buf, sizeof buf, "Not enough free space: about %.1f GB is needed", double(needed) / 1e9);
            return fail(buf);
        }
        const std::string stage = modsDir + ".hd-cutscenes-staging", cache = modsDir + ".downloads/hd-cutscenes";
        fs::remove_all(stage, ec);
        fs::create_directories(stage + "/files/data", ec);
        fs::create_directories(cache, ec);
        const std::string curl = curlPath();
        for (int i = 0; i < n && !cancel; i++) {
            const PortHdMovie& m = movies[i];
            movie = i + 1;
            step = 0;
            const std::string patch = cache + "/" + m.patch_file;
            char hex[65] = "";
            bool have = fileSize(patch) == m.patch_bytes && port_hd_sha256_file(patch.c_str(), hex) &&
                        !strcmp(hex, m.patch_sha256);
            if (!have) {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    part = patch + ".part";
                }
                if (fileSize(part) > m.patch_bytes) fs::remove(part, ec);
                Process p;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (cancel) break;
                    if (!p.start({curl, "-L", "-f", "-sS", "--retry", "3", "-C", "-", "-o", part, m.url}))
                        return fail("Cannot run curl to download the HD cutscenes");
                    child = &p;
                }
                std::string line, last;
                while (p.line(line))
                    if (!line.empty()) last = line;
                const int rc = p.wait();
                {
                    std::lock_guard<std::mutex> lk(mu);
                    child = nullptr;
                }
                if (cancel) break;
                if (rc != 0 || fileSize(part) != m.patch_bytes)
                    return fail("Downloading " + std::string(m.patch_file) + " failed" + (last.empty() ? "" : ": " + last));
                {  // the progress bar reads `part`: rename and forget it together
                    std::lock_guard<std::mutex> lk(mu);
                    fs::remove(patch, ec);
                    fs::rename(part, patch, ec);
                    part.clear();
                }
                if (ec) return fail("Cannot move " + std::string(m.patch_file) + " into place: " + ec.message());
                if (!port_hd_sha256_file(patch.c_str(), hex) || strcmp(hex, m.patch_sha256)) {
                    fs::remove(patch, ec);
                    return fail(std::string(m.patch_file) + " failed its checksum; try again");
                }
            }
            if (cancel) break;
            step = 1;
            char err[256] = "";
            const std::string out = stage + "/files/" + m.disc_path;
            if (!port_hd_apply(disc.c_str(), &m, patch.c_str(), out.c_str(), err, sizeof err))
                return fail(std::string(m.disc_path).substr(5) + ": " + err);
            fs::remove(patch, ec);
            doneBytes += m.patch_bytes;
        }
        if (cancel) {
            fs::remove_all(stage, ec);
            return fail("");
        }
        // the record and marker the game checks, then the swap
        if (FILE* f = openUtf8(stage + "/installed.json", "w")) {
            fputs(port_hd_catalog_json(), f);
            fclose(f);
        }
        if (FILE* f = openUtf8(stage + "/" + kHdMarker, "w")) {
            fprintf(f, "%s\n", release);
            fclose(f);
        }
        const std::string backup = modsDir + ".hd-cutscenes-previous";
        fs::remove_all(backup, ec);
        if (isDir(packDir())) fs::rename(packDir(), backup, ec);
        fs::rename(stage, packDir(), ec);
        if (ec) {
            if (isDir(backup)) fs::rename(backup, packDir(), ec);
            return fail("Cannot move the HD cutscenes into place");
        }
        fs::remove_all(backup, ec);
        phase = DONE;
    }
    void uninstall() {
        std::error_code ec;
        std::filesystem::remove_all(packDir(), ec);
        if (ec) return fail("Cannot remove " + packDir() + ": " + ec.message());
        phase = DONE;
    }
};

int countTextures(const std::string& dir) {  // tex1_* files below dir
    std::error_code ec;
    int n = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename().string().compare(0, 5, "tex1_") == 0) n++;
    return n;
}

struct Launcher {
    SettingsFile settings;
    BindingsFile bindings;
    std::string baseDir;
    Page page = P_DISPLAY;
    // installer
    std::string discSource;  // SMS_LAUNCHER_DISC: the game comes from elsewhere
    std::string installed;   // the image the game will use, "" when none
    char pickPath[1024] = "";
    std::string pickedFor;   // pickPath when it was last checked
    DiscCheck picked;
    InstallJob job;
    UpdateJob upd;
    std::string exeDir;  // where sms.exe (or the AppImage) is
    bool updateDismissed = false, updateTried = false;
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    float scale = 1.0f;
    int capture = -1;       // control waiting for a key press
    bool captureAdd = false;
    std::vector<std::string> displayNames;
    std::vector<std::string> fullscreenModes;  // "WxH@Hz" for the chosen display
    int modesFor = -1;
    std::vector<std::string> mods;
    int texturePacks = 0;   // folders in mods/textures
    int packTextures = 0;   // textures in the UHD pack's folder, mods/textures/GMS
    TexturePackJob tex;
    HdCutsceneJob hd;
    bool hdInstalled = false; // mods/hd-cutscenes holds a complete pack
    std::string status;
    double statusUntil = 0;

    // --- one row: label and help on the left, the control on the right
    float rowControlX() const { return ImGui::GetContentRegionAvail().x * 0.48f; }
    void rowBegin(const char* label, const char* help) {
        ImGui::PushID(label);
        ImGui::BeginGroup();
        const float x0 = ImGui::GetCursorPosX();
        const float wrap = rowControlX() - 16.0f * scale;
        ImGui::PushFont(bold, 0.0f);
        ImGui::TextUnformatted(label);
        ImGui::PopFont();
        if (help && *help) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushTextWrapPos(x0 + wrap);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
            ImGui::TextWrapped("%s", help);
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();
        const float labelBottom = ImGui::GetItemRectMax().y;
        ImGui::SameLine(x0 + rowControlX());
        ImGui::BeginGroup();
        rowLabelBottom = labelBottom;
    }
    float rowLabelBottom = 0;
    void rowEnd() {
        ImGui::EndGroup();
        const float bottom = std::max(rowLabelBottom, ImGui::GetItemRectMax().y);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, bottom + 8.0f * scale));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy(ImVec2(0, 8.0f * scale));
        ImGui::PopID();
    }

    // segmented buttons for a few options, a combo for many
    bool choice(const char* label, const char* help, const char* key, const char* def,
                const std::vector<Option>& opts, std::string* outValue = nullptr) {
        std::string cur = settings.get(key, def);
        bool changed = false;
        rowBegin(label, help);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (opts.size() <= 4) {
            const float w = (avail - ImGui::GetStyle().ItemSpacing.x * float(opts.size() - 1)) / float(opts.size());
            for (size_t i = 0; i < opts.size(); i++) {
                if (i) ImGui::SameLine();
                const bool on = cur == opts[i].value;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
                }
                if (ImGui::Button(opts[i].label, ImVec2(w, 0)) && !on) {
                    settings.set(key, opts[i].value);
                    changed = true;
                }
                if (on) ImGui::PopStyleColor(4);
            }
        } else {
            const char* preview = cur.c_str();
            for (const Option& o : opts)
                if (cur == o.value) preview = o.label;
            ImGui::SetNextItemWidth(avail);
            if (ImGui::BeginCombo("##c", preview, ImGuiComboFlags_HeightLarge)) {
                for (const Option& o : opts) {
                    const bool on = cur == o.value;
                    if (ImGui::Selectable(o.label, on)) {
                        settings.set(key, o.value);
                        changed = true;
                    }
                    if (on) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        rowEnd();
        if (outValue) *outValue = settings.get(key, def);
        return changed;
    }

    void toggle(const char* label, const char* help, const char* key, bool def) {
        std::string cur = settings.get(key, def ? "on" : "off");
        bool on = cur == "on" || cur == "1" || cur == "yes" || cur == "true";
        choice(label, help, key, def ? "on" : "off", {{"off", "Off"}, {"on", "On"}});
        (void)on;
    }

    void sliderInt(const char* label, const char* help, const char* key, int def, int lo, int hi,
                   const char* fmt, int step = 1) {
        int v = atoi(settings.get(key, std::to_string(def).c_str()).c_str());
        rowBegin(label, help);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::SliderInt("##s", &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) {
            if (step > 1) v = (v + step / 2) / step * step;
            settings.set(key, std::to_string(v));
        }
        rowEnd();
    }

    void sliderFloat(const char* label, const char* help, const char* key, float def, float lo, float hi,
                     const char* fmt) {
        float v = float(atof(settings.get(key, "").c_str()));
        if (settings.get(key, "").empty()) v = def;
        rowBegin(label, help);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::SliderFloat("##s", &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.2f", double(v));
            settings.set(key, buf);
        }
        rowEnd();
    }

    void info(const char* label, const char* text) {
        rowBegin(label, nullptr);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        rowEnd();
    }

    // --- environment
    void scanDisplays() {
        displayNames.clear();
        for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
            SDL_DisplayMode m;
            char buf[160];
            const char* n = SDL_GetDisplayName(i);
            if (SDL_GetDesktopDisplayMode(i, &m) == 0)
                snprintf(buf, sizeof buf, "%d: %s (%dx%d @ %d Hz)", i + 1, n ? n : "Display", m.w, m.h, m.refresh_rate);
            else
                snprintf(buf, sizeof buf, "%d: %s", i + 1, n ? n : "Display");
            displayNames.push_back(buf);
        }
    }
    int chosenDisplay() const {
        int d = atoi(settings.get("display", "0").c_str());
        return d >= 0 && d < int(displayNames.size()) ? d : 0;
    }
    void scanModes(int display) {
        if (modesFor == display) return;
        modesFor = display;
        fullscreenModes.clear();
        for (int i = 0; i < SDL_GetNumDisplayModes(display); i++) {
            SDL_DisplayMode m;
            if (SDL_GetDisplayMode(display, i, &m) != 0) continue;
            char buf[48];
            snprintf(buf, sizeof buf, "%dx%d@%d", m.w, m.h, m.refresh_rate);
            if (std::find(fullscreenModes.begin(), fullscreenModes.end(), buf) == fullscreenModes.end())
                fullscreenModes.push_back(buf);
        }
    }
    void scanMods() {
        mods.clear();
        const std::string dir = baseDir + "mods";
        for (const std::string& d : listDirs(dir))
            if (d != "textures" && d != "hd-cutscenes" && isDir(dir + "/" + d + "/files")) mods.push_back(d);
        texturePacks = int(listDirs(dir + "/textures").size());
        packTextures = isDir(dir + "/textures/GMS") ? countTextures(dir + "/textures/GMS") : 0;
        hdInstalled = fileSize(dir + "/hd-cutscenes/" + kHdMarker) > 0;
    }

    // --- pages
    void pageDisplay() {
        std::string mode;
        choice("Window mode",
               "Borderless fills the screen at your desktop resolution and switches instantly. Exclusive fullscreen "
               "takes over the display. F11 or Alt+Enter toggles fullscreen while playing.",
               "window_mode", "windowed",
               {{"windowed", "Windowed"}, {"borderless", "Borderless"}, {"fullscreen", "Exclusive"}}, &mode);
        if (displayNames.size() > 1) {
            std::vector<std::string> values(displayNames.size());
            std::vector<Option> opts;
            for (size_t i = 0; i < displayNames.size(); i++) {
                values[i] = std::to_string(i);
                opts.push_back({values[i].c_str(), displayNames[i].c_str()});
            }
            choice("Monitor", "The display the game opens on.", "display", "0", opts);
        } else if (!displayNames.empty()) {
            info("Monitor", displayNames[0].c_str());
        }
        if (mode == "fullscreen") {
            scanModes(chosenDisplay());
            std::vector<Option> opts = {{"desktop", "Desktop resolution"}};
            std::vector<std::string> labels(fullscreenModes.size());
            for (size_t i = 0; i < fullscreenModes.size(); i++) {
                int w = 0, h = 0, hz = 0;
                sscanf(fullscreenModes[i].c_str(), "%dx%d@%d", &w, &h, &hz);
                char buf[64];
                snprintf(buf, sizeof buf, "%d x %d  @ %d Hz", w, h, hz);
                labels[i] = buf;
                opts.push_back({fullscreenModes[i].c_str(), labels[i].c_str()});
            }
            choice("Fullscreen resolution", "The display mode used in exclusive fullscreen.", "fullscreen_mode",
                   "desktop", opts);
        } else if (mode == "windowed") {
            choice("Window size", "The size the window opens at. It can be resized or maximized while playing.",
                   "window_scale", "0",
                   {{"0", "Automatic"}, {"1", "640 x 480"}, {"2", "1280 x 960"}, {"3", "1920 x 1440"},
                    {"4", "2560 x 1920"}});
        }
        choice("Vertical sync",
               "Waits for the display before showing a frame, which stops tearing. Adaptive only tears when a frame "
               "is late.",
               "vsync", "off", {{"off", "Off"}, {"on", "On"}, {"adaptive", "Adaptive"}});

        // widescreen: "auto" writes the chosen display's own aspect ratio
        std::string ws = settings.get("widescreen", "off");
        if (ws == "on" || ws == "1") settings.set("widescreen", "16:9");
        std::string autoAspect;
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(chosenDisplay(), &dm) == 0 && dm.h > 0) {
            int a = dm.w, b = dm.h;
            while (b) { int t = a % b; a = b; b = t; }
            autoAspect = std::to_string(dm.w / a) + ":" + std::to_string(dm.h / a);
        }
        std::vector<Option> wopts = {{"off", "Off (4:3, original)"}, {"16:9", "16:9"}, {"16:10", "16:10"},
                                     {"21:9", "21:9 ultrawide"}, {"32:9", "32:9 super ultrawide"}};
        std::string autoLabel = "Match monitor (" + autoAspect + ")";
        bool known = false;
        for (const Option& o : wopts) known = known || autoAspect == o.value;
        if (!autoAspect.empty() && !known) wopts.push_back({autoAspect.c_str(), autoLabel.c_str()});
        choice("Widescreen",
               "Shows more of the world to the sides instead of stretching. Menus and the HUD keep their 4:3 shape.",
               "widescreen", "off", wopts, &ws);
        if (ws != "off" && ws != "0")
            choice("HUD position", "In widescreen, keep the HUD in the middle or move the counters to the edges.",
                   "widescreen_hud", "centre", {{"centre", "Centre"}, {"edges", "Screen edges"}});
        choice("Aspect ratio", "Keep the correct shape with black bars, stretch to fill the window, or use whole "
               "multiples of the original picture.",
               "aspect", "keep", {{"keep", "Keep"}, {"stretch", "Stretch"}, {"integer", "Integer"}});
        choice("Scaling filter",
               "How the picture is scaled to the window. Smooth averages extra pixels when supersampling; Sharp keeps "
               "crisp pixel edges; Nearest is unfiltered.",
               "present_filter", "bilinear", {{"bilinear", "Smooth"}, {"sharp", "Sharp"}, {"nearest", "Nearest"}});
    }

    void pageGraphics() {
        SDL_DisplayMode dm;
        int recommend = 2;
        if (SDL_GetDesktopDisplayMode(chosenDisplay(), &dm) == 0) recommend = std::max(1, (dm.h + 527) / 528);
        recommend = std::min(recommend, 8);
        static const char* const kRes[] = {
            "1x  -  640 x 528 (original)", "2x  -  1280 x 1056 (720p+)", "3x  -  1920 x 1584 (1080p+)",
            "4x  -  2560 x 2112 (1440p+)", "5x  -  3200 x 2640 (4K)", "6x  -  3840 x 3168 (4K+)",
            "7x  -  4480 x 3696 (5K)", "8x  -  5120 x 4224 (8K-class)"};
        std::vector<std::string> labels(8), values(8);
        std::vector<Option> opts;
        for (int i = 0; i < 8; i++) {
            values[i] = std::to_string(i + 1);
            labels[i] = kRes[i];
            if (i + 1 == recommend) labels[i] += "   - recommended";
            opts.push_back({values[i].c_str(), labels[i].c_str()});
        }
        char help[256];
        snprintf(help, sizeof help,
                 "The resolution the game renders at, in multiples of the GameCube's 640 x 528. %dx matches your "
                 "monitor; higher values supersample for an even cleaner image.",
                 recommend);
        choice("Internal resolution", help, "resolution", "1", opts);
        choice("Anti-aliasing (MSAA)", "Smooths the jagged edges of 3D geometry. 4x is a good balance.", "msaa", "0",
               {{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}});
        toggle("FXAA", "A fast post-process edge smoother. Also softens edges MSAA misses, such as foliage.", "fxaa",
               false);
        choice("Anisotropic filtering", "Keeps ground and wall textures sharp at steep angles.", "anisotropic", "0",
               {{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}});
        sliderInt("Sharpening", "Contrast-adaptive sharpening of the final picture.", "sharpen", 0, 0, 100, "%d%%");
        sliderFloat("Brightness", "1.00 is the original image.", "brightness", 1.0f, 0.5f, 2.0f, "%.2f");
        textureRows();
    }

    // --- HD texture pack: install, then on/off
    void textureRows() {
        // a finished job: rescan, and switch packs on after an install
        const int ph = tex.phase;
        if (ph == TexturePackJob::DONE || ph == TexturePackJob::FAILED || ph == TexturePackJob::CANCELLED) {
            const bool installedNow = ph == TexturePackJob::DONE && isDir(tex.installDir());
            if (ph == TexturePackJob::FAILED) status = "Texture pack: " + tex.error;
            else if (ph == TexturePackJob::CANCELLED) status = "Texture pack install cancelled.";
            else status = installedNow ? "HD texture pack installed and switched on." : "HD texture pack removed.";
            statusUntil = ImGui::GetTime() + 8.0;
            if (installedNow) settings.set("texture_packs", "on");
            tex.phase = TexturePackJob::IDLE;
            scanMods();
        }
        rowBegin("HD texture pack",
                 "The Super Mario Sunshine UHD Texture Pack by qashto and razius: over 2,000 remade textures. "
                 "Downloaded from its GitHub release (940 MB) and installed into mods/textures (3.2 GB).");
        const float w = ImGui::GetContentRegionAvail().x;
        if (tex.busy()) {
            const char* what = tex.phase == TexturePackJob::DOWNLOADING ? "Downloading"
                               : tex.phase == TexturePackJob::EXTRACTING ? "Unpacking"
                                                                         : "Removing";
            char label[64];
            if (tex.phase == TexturePackJob::REMOVING) snprintf(label, sizeof label, "%s...", what);
            else snprintf(label, sizeof label, "%s... %.0f%%", what, double(tex.progress()) * 100.0);
            ImGui::ProgressBar(tex.phase == TexturePackJob::REMOVING ? -1.0f * float(ImGui::GetTime()) : tex.progress(),
                               ImVec2(w * 0.68f, 0), label);
            ImGui::SameLine();
            ImGui::BeginDisabled(tex.phase == TexturePackJob::REMOVING);
            if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) tex.stop();
            ImGui::EndDisabled();
        } else if (packTextures > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.92f, 0.65f, 1));
            ImGui::Text("Installed: %d textures", packTextures);
            ImGui::PopStyleColor();
            if (ImGui::Button("Remove", ImVec2(w * 0.5f - ImGui::GetStyle().ItemSpacing.x / 2, 0)))
                ImGui::OpenPopup("Remove the HD texture pack?");
            ImGui::SameLine();
            if (ImGui::Button("Reinstall", ImVec2(-FLT_MIN, 0))) startTextureInstall();
            if (ImGui::BeginPopupModal("Remove the HD texture pack?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::TextUnformatted("This deletes mods/textures/GMS (3.2 GB). It can be downloaded again.");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Remove", ImVec2(160 * scale, 0))) {
                    tex.begin(&TexturePackJob::uninstall);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep it", ImVec2(160 * scale, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
            if (ImGui::Button("Download and install", ImVec2(w, 0))) startTextureInstall();
            ImGui::PopStyleColor(4);
        }
        rowEnd();
        if (packTextures > 0 || texturePacks > 0) {
            char help[160];
            snprintf(help, sizeof help, "Use the texture packs in mods/textures (%d installed).", texturePacks);
            toggle("Use HD textures", help, "texture_packs", true);
            sliderInt("Texture pack memory",
                      "Video memory kept for texture pack images, in MiB, before the least used are freed.",
                      "texture_pack_mb", 1536, 512, 8192, "%d MiB", 256);
        }
        cutsceneRows();
    }

    // --- HD cutscenes: install, then on/off
    std::string discForInstall() const {
        if (discSource == "bundled" || installed.compare(0, 8, "Built in") == 0) return "bundled";
        return discSource.empty() ? installed : discSource;
    }

    void cutsceneRows() {
        const int ph = hd.phase;
        if (ph == HdCutsceneJob::DONE || ph == HdCutsceneJob::FAILED || ph == HdCutsceneJob::CANCELLED) {
            scanMods();
            if (ph == HdCutsceneJob::FAILED) status = "HD cutscenes: " + hd.error;
            else if (ph == HdCutsceneJob::CANCELLED) status = "HD cutscene install cancelled.";
            else status = hdInstalled ? "HD cutscenes installed and switched on." : "HD cutscenes removed.";
            statusUntil = ImGui::GetTime() + 8.0;
            if (ph == HdCutsceneJob::DONE && hdInstalled) settings.set("hd_cutscenes", "on");
            hd.phase = HdCutsceneJob::IDLE;
        }
        rowBegin("HD cutscenes",
                 "All 21 movies enhanced to 3x resolution with AI upscaling, with their original timing and audio. "
                 "Built from your own disc image: about 5.7 GB to download, 5.8 GB installed.");
        const float w = ImGui::GetContentRegionAvail().x;
        if (hd.busy()) {
            char label[96];
            if (hd.phase == HdCutsceneJob::REMOVING) snprintf(label, sizeof label, "Removing...");
            else if (hd.step == 0)
                snprintf(label, sizeof label, "Downloading movie %d of 21... %.0f%%", int(hd.movie),
                         double(hd.progress()) * 100.0);
            else snprintf(label, sizeof label, "Preparing movie %d of 21...", int(hd.movie));
            ImGui::ProgressBar(hd.phase == HdCutsceneJob::REMOVING ? -1.0f * float(ImGui::GetTime()) : hd.progress(),
                               ImVec2(w * 0.68f, 0), label);
            ImGui::SameLine();
            ImGui::BeginDisabled(hd.phase == HdCutsceneJob::REMOVING);
            if (ImGui::Button("Cancel##hd", ImVec2(-FLT_MIN, 0))) hd.stop();
            ImGui::EndDisabled();
        } else if (hdInstalled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.92f, 0.65f, 1));
            ImGui::TextUnformatted("Installed: 21 HD movies");
            ImGui::PopStyleColor();
            if (ImGui::Button("Remove##hd", ImVec2(w * 0.5f - ImGui::GetStyle().ItemSpacing.x / 2, 0)))
                ImGui::OpenPopup("Remove the HD cutscenes?");
            ImGui::SameLine();
            if (ImGui::Button("Reinstall##hd", ImVec2(-FLT_MIN, 0))) startCutsceneInstall();
            if (ImGui::BeginPopupModal("Remove the HD cutscenes?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::TextUnformatted("This deletes mods/hd-cutscenes (5.8 GB). They can be installed again.");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Remove", ImVec2(160 * scale, 0))) {
                    hd.modsDir = baseDir + "mods/";
                    hd.begin(&HdCutsceneJob::uninstall);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep them", ImVec2(160 * scale, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        } else {
            ImGui::BeginDisabled(installed.empty());
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
            if (ImGui::Button(installed.empty() ? "Install the game first##hd" : "Download and install##hd",
                              ImVec2(w, 0)))
                startCutsceneInstall();
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
        }
        rowEnd();
        if (hdInstalled)
            toggle("Use HD cutscenes", "Play the enhanced movies instead of the originals.", "hd_cutscenes", true);
    }

    void startCutsceneInstall() {
        hd.modsDir = baseDir + "mods/";
        hd.disc = discForInstall();
        hd.begin(&HdCutsceneJob::install);
    }

    void startTextureInstall() {
        tex.modsDir = baseDir + "mods/";
        if (char* base = SDL_GetBasePath()) {
            tex.exeDir = base;
            SDL_free(base);
        }
        tex.begin(&TexturePackJob::install);
    }

    void pageGameplay() {
        choice("Frame rate", "60 runs gameplay at twice the original frame rate, at the game's normal speed. "
               "Menus and movies stay at 30.",
               "frame_rate", "30", {{"30", "30 fps (original)"}, {"60", "60 fps"}});
        toggle("Skip intro movies", "Go straight to the title screen.", "skip_movies", false);
        toggle("Performance overlay", "Show the frame rate and timings at start. Toggle in game with the ` key.",
               "overlay", false);
        std::vector<Option> opts = {{"none", "None"}};
        for (const std::string& m : mods) opts.push_back({m.c_str(), m.c_str()});
        choice("Game mod", mods.empty() ? "No mods found in mods/. See mods/README.md."
                                        : "Game file mods from mods/<name>/files.",
               "mod", "none", opts);
    }

    // a text setting, written when it changes
    void textField(const char* label, const char* help, const char* key, const char* def, const char* hint) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", settings.get(key, def).c_str());
        rowBegin(label, help);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputTextWithHint("##t", hint, buf, sizeof buf)) settings.set(key, trim(buf));
        rowEnd();
    }

    std::string localAddresses;
    bool addressesRead = false;

    void pageOnline() {
        std::string mode;
        choice("Online play",
               "Host a game for friends to join, or join one. Everyone needs this version of the port and their "
               "own copy of the game. Other players appear when you are in the same level and episode.",
               "net_mode", "off", {{"off", "Off"}, {"host", "Host"}, {"join", "Join"}}, &mode);
        if (mode == "off") return;
        textField("Your name", "Shown to the other players (up to 15 letters).", "net_name", "", "Mario");
        if (mode == "join") {
            textField("Host address", "The IP address or name of the computer hosting the game.", "net_address", "",
                      "for example 192.168.1.20");
        } else {
            if (!addressesRead && port_net_local_addresses) {
                char buf[256] = "";
                port_net_local_addresses(buf, sizeof buf);
                localAddresses = buf;
                addressesRead = true;
            }
            info("Your address",
                 (localAddresses.empty() ? std::string("(not found)") : localAddresses).c_str());
            info("Playing over the internet",
                 "Players on your home network join with the address above. For friends elsewhere, forward the UDP "
                 "port below to this computer on your router and give them your public IP address.");
        }
        textField("Port", "The UDP port the game uses. The host and the players joining must use the same one.",
                  "net_port", "27016", "27016");
    }

    void pageAudio() {
        toggle("Sound", "Turn all sound output on or off.", "audio", true);
        sliderInt("Master volume", nullptr, "volume", 100, 0, 100, "%d%%");
    }

    void pageControls() {
        int pads = 0;
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i)) {
                const char* n = SDL_GameControllerNameForIndex(i);
                info(pads++ ? "" : "Controller", n ? n : "Game controller");
            }
        if (!pads) info("Controller", "None connected. Any controller SDL recognises works: plug it in at any time.");
        for (int i = 0; i < kNumControls; i++) {
            ImGui::PushID(i);
            rowBegin(kControlLabels[i], nullptr);
            const float w = ImGui::GetContentRegionAvail().x;
            std::string shown;
            {
                std::string s = bindings.keys[i];
                size_t p = 0;
                while (p < s.size()) {
                    size_t q = s.find(' ', p);
                    std::string k = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    if (!k.empty()) shown += (shown.empty() ? "" : "  /  ") + prettyKey(k);
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
            }
            if (capture == i) {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
                ImGui::Button(captureAdd ? "Press a key to add..." : "Press a key...", ImVec2(w * 0.6f, 0));
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) capture = -1;
            } else {
                if (ImGui::Button(shown.empty() ? "(none)" : shown.c_str(), ImVec2(w * 0.6f, 0))) {
                    capture = i;
                    captureAdd = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Add", ImVec2((w * 0.4f - ImGui::GetStyle().ItemSpacing.x * 2) / 2, 0))) {
                    capture = i;
                    captureAdd = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset", ImVec2(-FLT_MIN, 0))) bindings.keys[i] = kDefaultKeys[i];
            }
            rowEnd();
            ImGui::PopID();
        }
    }

    // --- installer
    int installMode = 0;  // 0: copy into rom/, 1: use the image where it is
    double suppressClicks = 0;  // mouse buttons are ignored until then (after a file dialog)

    static std::string absPath(const std::string& p) {
#ifdef _WIN32
        wchar_t w[1024], full[1024];
        if (!MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, 1024) || !_wfullpath(full, w, 1024)) return p;
        char out[2048];
        WideCharToMultiByte(CP_UTF8, 0, full, -1, out, sizeof out, nullptr, nullptr);
        std::string s = out;
        for (char& c : s)
            if (c == '\\') c = '/';
        return s;
#else
        char buf[4096];
        return realpath(p.c_str(), buf) ? std::string(buf) : p;
#endif
    }

    void refreshInstalled() {
        installed.clear();
        if (!discSource.empty()) {
            installed = discSource == "bundled" ? "Built into this executable" : discSource;
            return;
        }
        const std::string img = settings.get("disc_image", "");
        if (!img.empty() && fileSize(img) > 0) {
            installed = img;
            return;
        }
        std::vector<std::string> found;
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((baseDir + "rom\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.push_back(baseDir + "rom/" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        if (DIR* d = opendir((baseDir + "rom").c_str())) {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.') found.push_back(baseDir + "rom/" + e->d_name);
            closedir(d);
        }
#endif
        std::vector<std::string> ok;
        for (const std::string& f : found)
            if (checkDisc(f).ok) ok.push_back(f);
        if (ok.size() == 1) installed = ok[0];
    }

    void statusCard() {
        const bool ready = !installed.empty();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 78.0f * scale;
        dl->AddRectFilled(p, p + ImVec2(w, h), ready ? IM_COL32(28, 110, 72, 255) : IM_COL32(150, 82, 20, 255),
                          12.0f * scale);
        dl->AddCircleFilled(p + ImVec2(38 * scale, h / 2), 16 * scale,
                            ready ? IM_COL32(120, 230, 160, 255) : IM_COL32(255, 200, 90, 255), 24);
        ImGui::SetCursorScreenPos(p + ImVec2(70 * scale, 14 * scale));
        ImGui::BeginGroup();
        ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextUnformatted(ready ? "Ready to play" : "Game not installed");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(p.x + w - 16 * scale);
        ImGui::TextUnformatted(ready ? installed.c_str() : "Select your disc image below to install it.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(p + ImVec2(0, h + 16 * scale));
    }

    void pageInstall() {
        statusCard();
        if (!discSource.empty()) {
            info("Game source", discSource == "bundled"
                                    ? "This executable carries the game's files, so nothing needs installing."
                                    : "The game image was given on the command line or by SMS_DISC_IMAGE.");
            return;
        }
        rowBegin("Disc image",
                 "Your own Super Mario Sunshine disc: North America (GMSE01), revision 0, as an ISO, GCM, NKit "
                 "ISO or Dolphin CISO. You can also drop the file onto this window.");
        const float bw = 130.0f * scale;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##path", "Path to the disc image", pickPath, sizeof pickPath);
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(bw, 0)) && !job.running) {
            const std::string s = browseForImage();
            if (!s.empty()) snprintf(pickPath, sizeof pickPath, "%s", s.c_str());
            // the click that closed the dialog must not land on a button here
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            suppressClicks = ImGui::GetTime() + 0.35;
        }
        const std::string path = trim(pickPath);
        if (path != pickedFor) {
            pickedFor = path;
            picked = path.empty() ? DiscCheck() : checkDisc(path);
        }
        if (!path.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, picked.ok ? ImVec4(0.55f, 0.92f, 0.65f, 1) : ImVec4(1, 0.72f, 0.4f, 1));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(picked.summary.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        rowEnd();

        rowBegin("Install method",
                 "Copying puts the image in the game's rom folder (about 1.2 GB), so it keeps working if the original "
                 "moves. Using it in place needs no space but the file must stay where it is.");
        const char* modes[] = {"Copy into the game folder", "Use it where it is"};
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine();
            const bool on = installMode == i;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
            }
            if (ImGui::Button(modes[i], ImVec2(half, 0))) installMode = i;
            if (on) ImGui::PopStyleColor(3);
        }
        rowEnd();

        if (job.running) {
            const float frac = job.total > 0 ? float(double(job.copied) / double(job.total)) : 0.0f;
            char label[64];
            snprintf(label, sizeof label, "Installing... %.0f%%", double(frac) * 100.0);
            ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x - 150 * scale, 44 * scale), label);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 44 * scale))) job.cancel = true;
            return;
        }
        if (job.done) {
            job.done = false;
            settings.set("disc_image", absPath(job.dest));
            settings.save();
            refreshInstalled();
            status = "Installed. Press Play to start the game.";
            statusUntil = ImGui::GetTime() + 6.0;
        }
        if (job.failed) {
            job.failed = false;
            status = "Install failed: " + job.error;
            statusUntil = ImGui::GetTime() + 8.0;
        }
        ImGui::BeginDisabled(!picked.ok);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
        if (ImGui::Button(installed.empty() ? "Install" : "Install this image instead", ImVec2(320 * scale, 48 * scale))) {
            if (installMode == 1) {
                settings.set("disc_image", absPath(path));
                settings.save();
                refreshInstalled();
                status = "Done. Press Play to start the game.";
                statusUntil = ImGui::GetTime() + 6.0;
            } else {
                std::string ext = path.substr(path.find_last_of('.') + 1);
                for (char& c : ext) c = char(tolower(c));
                std::string lower = path;
                for (char& c : lower) c = char(tolower(c));
                if (lower.size() > 9 && lower.compare(lower.size() - 9, 9, ".nkit.iso") == 0) ext = "nkit.iso";
                const std::string dir = baseDir + "rom";
#ifdef _WIN32
                _mkdir(dir.c_str());
#else
                mkdir(dir.c_str(), 0755);
#endif
                job.start(path, dir + "/GMSE01." + ext);
            }
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
    }

    void pageCamera() {
        toggle("Invert horizontal (X)", "Flip left and right camera movement, on the C-stick, right stick and mouse.",
               "camera_invert_x", false);
        toggle("Invert vertical (Y)", "Flip up and down camera movement.", "camera_invert_y", false);
        toggle("Free camera",
               "The camera stays where you point it instead of swinging back behind Mario as he runs, like the free "
               "camera of the Super Mario 64 PC port. Press L to recentre it.",
               "free_camera", false);
        sliderInt("Camera speed", "How fast the camera turns with the stick. 100% is the original speed.",
                  "camera_speed", 100, 25, 300, "%d%%", 5);
        toggle("Mouse look",
               "Turn the camera with the mouse. The game captures the mouse while it has focus: F10 releases it, "
               "click the window to take it back.",
               "mouse_camera", false);
        sliderInt("Mouse sensitivity", nullptr, "mouse_sensitivity", 100, 10, 500, "%d%%", 5);
    }

    // --- updates and the official launcher
    void startUpdateCheck() {
        if (!upd.busy()) upd.begin(&UpdateJob::check);
    }

    bool accentButton(const char* label, ImVec2 size) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
        const bool r = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return r;
    }

    // the update controls, on the banner and the About page
    void updateControls(bool& quit, bool banner) {
        const float w = ImGui::GetContentRegionAvail().x;
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
        switch (upd.phase.load()) {
        case UpdateJob::AVAILABLE:
            if (UpdateJob::canInstall(exeDir)) {
                if (accentButton("Update now##upd", ImVec2(banner ? half : w * 0.5f, 0))) {
                    updateTried = true;
                    upd.begin(&UpdateJob::install);
                }
            } else if (accentButton("Open the download page##upd", ImVec2(banner ? half : w * 0.5f, 0))) {
                SDL_OpenURL(kReleasesPage);
            }
            ImGui::SameLine();
            if (banner) {
                if (ImGui::Button("Later##upd", ImVec2(-FLT_MIN, 0))) updateDismissed = true;
            } else if (ImGui::Button("What's new##upd", ImVec2(-FLT_MIN, 0))) {
                SDL_OpenURL(kReleasesPage);
            }
            break;
        case UpdateJob::DOWNLOADING:
        case UpdateJob::INSTALLING: {
            char label[96];
            if (upd.phase == UpdateJob::INSTALLING) snprintf(label, sizeof label, "Installing...");
            else snprintf(label, sizeof label, "Downloading %s... %.0f%%", upd.latest.c_str(),
                          double(upd.progress()) * 100.0);
            ImGui::ProgressBar(upd.phase == UpdateJob::INSTALLING ? -1.0f * float(ImGui::GetTime()) : upd.progress(),
                               ImVec2(w * 0.68f, 0), label);
            ImGui::SameLine();
            ImGui::BeginDisabled(upd.phase == UpdateJob::INSTALLING);
            if (ImGui::Button("Cancel##upd", ImVec2(-FLT_MIN, 0))) upd.stop();
            ImGui::EndDisabled();
            break;
        }
        case UpdateJob::READY:
            if (accentButton("Restart now##upd", ImVec2(w, 0))) {
                settings.save();
                bindings.save();
                if (restartSelf()) quit = true;
                else {
                    status = "Updated. Start the game again to use the new version.";
                    statusUntil = ImGui::GetTime() + 8.0;
                }
            }
            break;
        default:
            if (!banner) {
                ImGui::BeginDisabled(upd.busy());
                if (ImGui::Button(upd.phase == UpdateJob::CHECKING ? "Checking...##upd" : "Check for updates##upd",
                                  ImVec2(w * 0.5f, 0)))
                    startUpdateCheck();
                ImGui::EndDisabled();
            }
            break;
        }
    }

    // a strip above the page: an update, or the note about the official launcher
    void drawBanner(bool& quit) {
        const int ph = upd.phase;
        const bool updating = ph == UpdateJob::DOWNLOADING || ph == UpdateJob::INSTALLING || ph == UpdateJob::READY;
        const bool showUpdate = updating || (ph == UpdateJob::AVAILABLE && !updateDismissed) ||
                                (ph == UpdateJob::FAILED && updateTried);
        const bool showNote = !showUpdate && settings.get("sms_launcher_notice", "") != "seen";
        if (!showUpdate && !showNote) return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 0.84f, 0.25f, 0.13f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * scale);
        ImGui::BeginChild("banner", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushTextWrapPos(0.0f);
        if (showUpdate) {
            ImGui::PushFont(bold, 0.0f);
            if (ph == UpdateJob::READY) ImGui::Text("Updated to %s", upd.latest.c_str());
            else if (ph == UpdateJob::FAILED) ImGui::TextUnformatted("The update failed");
            else ImGui::Text("Update available: %s (you have %s)", upd.latest.c_str(), kPortVersion);
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (ph == UpdateJob::READY)
                ImGui::TextUnformatted("Restart to play the new version. Your settings, game and mods are kept.");
            else if (ph == UpdateJob::FAILED) ImGui::TextUnformatted(upd.error.c_str());
            else ImGui::TextUnformatted("Your settings, installed game, HD textures and cutscenes are kept.");
            ImGui::PopStyleColor();
            if (ph != UpdateJob::FAILED) updateControls(quit, true);
            else if (ImGui::Button("Open the download page##updf")) SDL_OpenURL(kReleasesPage);
        } else {
            ImGui::PushFont(bold, 0.0f);
            ImGui::TextUnformatted("The official launcher is SMS Launcher");
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("chasem-dev, who made this port, has his own launcher. This one's features are "
                                   "being contributed to it, and this launcher will be retired once they are there. "
                                   "Until then it keeps working and keeps updating.");
            ImGui::PopStyleColor();
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
            if (accentButton("Get SMS Launcher##note", ImVec2(half, 0))) SDL_OpenURL(kSmsLauncherPage);
            ImGui::SameLine();
            if (ImGui::Button("Got it##note", ImVec2(-FLT_MIN, 0))) {
                settings.set("sms_launcher_notice", "seen");
                settings.save();
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6 * scale));
    }

    void pageAbout(bool& quit) {
        {
            char text[160];
            const int ph = upd.phase;
            if (ph == UpdateJob::LATEST) snprintf(text, sizeof text, "Version %s. You have the latest version.", kPortVersion);
            else if (ph == UpdateJob::AVAILABLE) snprintf(text, sizeof text, "Version %s. %s is available.", kPortVersion,
                                                           upd.latest.c_str());
            else if (ph == UpdateJob::FAILED) snprintf(text, sizeof text, "Version %s. %s", kPortVersion,
                                                        upd.error.c_str());
            else snprintf(text, sizeof text, "Version %s.", kPortVersion);
            rowBegin("Updates", text);
            updateControls(quit, false);
            rowEnd();
        }
        toggle("Check for updates at startup", "Looks for a new release on GitHub each time the launcher opens. "
               "Nothing is downloaded until you choose Update.", "update_check", true);
        rowBegin("Official launcher", "SMS Launcher, by the port's author, chasem-dev. This launcher's features are "
                 "being added to it; once they are, it is the one to use.");
        if (accentButton("Get SMS Launcher##about", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
            SDL_OpenURL(kSmsLauncherPage);
        rowEnd();
        toggle("Show this menu at startup",
               "When off, the game starts straight away. Hold Shift while starting it to see this menu again.",
               "launcher", true);
        info("Settings file", settings.path.c_str());
        info("Key bindings", bindings.path.c_str());
        info("In-game keys", "F11 or Alt+Enter: fullscreen.   ` (backtick): performance overlay.   "
             "F7 with the overlay open: game speed.   Esc: quit.");
        info("About", "Super Mario Sunshine PC port, built from the decompilation. The game itself is read from "
             "your own disc image. Launcher drawn with Dear ImGui.");
    }

    // --- the one screen left: SMS Launcher is the launcher now
    MoveJob move;

    // The memory card folder this game uses (platform/card/card.cpp): SMS_SAVE_DIR or save_dir (relative
    // to the game folder), else the default that SMS Launcher shares.
    std::string cardDir() {
        namespace fs = std::filesystem;
        std::string dir = getenv("SMS_SAVE_DIR") ? getenv("SMS_SAVE_DIR") : settings.get("save_dir", "");
        if (dir.empty()) return MoveJob::launcherCardDir();
        fs::path p(dir);
        return (p.is_absolute() ? p : fs::path(baseDir.empty() ? "." : baseDir) / p).string();
    }

    // SMS Launcher's look (its src/style.css): the beach background under two dark gradients, a
    // translucent navy top and status bar, a dark setup panel and a teal launch panel with the
    // big yellow button.
    unsigned bgTex = 0;
    int bgW = 0, bgH = 0;

    static ImU32 hex(unsigned rgb, int a = 255) {
        return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
    }

    // A button drawn as SMS Launcher draws them: 0 its yellow Play button (with the darker lip),
    // 1 the translucent outlined one on dark panels.
    bool smsButton(const char* label, ImVec2 size, int style, float fontSize, bool enabled = true) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::InvisibleButton(label, size);
        ImGui::EndDisabled();
        const bool hot = enabled && ImGui::IsItemHovered(), down = hot && ImGui::IsItemActive();
        const float r = 11.0f * scale;
        const ImVec2 q(p.x + size.x, p.y + size.y);
        if (style == 0) {
            const float lip = (down ? 1.0f : 4.0f) * scale, dy = down ? 3.0f * scale : 0.0f;
            dl->AddRectFilled(ImVec2(p.x, p.y + lip + dy), ImVec2(q.x, q.y + lip), hex(0xb98127), r);
            dl->AddRectFilled(ImVec2(p.x, p.y + dy), ImVec2(q.x, q.y + dy), hot ? hex(0xffda86) : hex(0xffc75a, enabled ? 255 : 190), r);
        } else {
            dl->AddRectFilled(p, q, hex(0xffffff, hot ? 0x38 : 0x20), r * 0.75f);
            dl->AddRect(p, q, hex(0xb5e6ee, 0x8c), r * 0.75f, 0, 1.0f * scale);
        }
        const char* end = strstr(label, "##") ? strstr(label, "##") : label + strlen(label);
        const ImVec2 ts = bold->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label, end);
        const float dy = style == 0 && down ? 3.0f * scale : 0.0f;
        dl->AddText(bold, fontSize, ImVec2(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2 + dy),
                    style == 0 ? hex(0x143749) : hex(0xffffff), label, end);
        return pressed && enabled;
    }

    // a word-wrapped line in one colour and size
    void wrapped(const char* text, ImU32 color, ImFont* font, float size) {
        ImGui::PushFont(font, size);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    bool movedFrame(bool& quit) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, hex(0x0a3045));
        ImGui::Begin("moved", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = vp->Pos;
        const float W = vp->Size.x, H = vp->Size.y, t = float(ImGui::GetTime());
        const float base = ImGui::GetStyle().FontSizeBase;

        // background: the picture, covering, centred at 40% across; then the two gradients
        if (bgTex && bgW > 0 && bgH > 0) {
            const float sa = float(bgW) / float(bgH), da = W / H;
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (da > sa) {  // wider than the picture: crop top and bottom
                const float v = sa / da;
                uv0.y = (1 - v) * 0.5f;
                uv1.y = uv0.y + v;
            } else {
                const float u = da / sa;
                uv0.x = (1 - u) * 0.4f;
                uv1.x = uv0.x + u;
            }
            dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)bgTex), o, ImVec2(o.x + W, o.y + H), uv0, uv1);
        }
        dl->AddRectFilledMultiColor(o, ImVec2(o.x + W, o.y + H * 0.31f), hex(0x07334b, 0x70), hex(0x07334b, 0x70),
                                    hex(0x07334b, 0x05), hex(0x07334b, 0x05));
        dl->AddRectFilledMultiColor(ImVec2(o.x, o.y + H * 0.31f), ImVec2(o.x + W, o.y + H), hex(0x062b42, 0x05),
                                    hex(0x062b42, 0x05), hex(0x062b42, 0xaa), hex(0x062b42, 0xaa));
        dl->AddRectFilledMultiColor(o, ImVec2(o.x + W * 0.47f, o.y + H), hex(0x072d45, 0xac), hex(0x072d45, 0x2e),
                                    hex(0x072d45, 0x2e), hex(0x072d45, 0xac));
        dl->AddRectFilledMultiColor(ImVec2(o.x + W * 0.47f, o.y), ImVec2(o.x + W * 0.8f, o.y + H), hex(0x072d45, 0x2e),
                                    hex(0x072d45, 0), hex(0x072d45, 0), hex(0x072d45, 0x2e));

        // top bar
        const float barH = 58 * scale, statusH = 36 * scale, padX = 28 * scale;
        dl->AddRectFilled(o, ImVec2(o.x + W, o.y + barH), hex(0x092e45, 0xbb));
        dl->AddLine(ImVec2(o.x, o.y + barH), ImVec2(o.x + W, o.y + barH), hex(0xffffff, 0x2a));
        const ImVec2 mark(o.x + 26 * scale, o.y + (barH - 30 * scale) / 2);
        dl->AddRectFilled(mark, ImVec2(mark.x + 30 * scale, mark.y + 30 * scale), hex(0xffc75a), 9 * scale);
        const char* star = "\xE2\x9C\xA6";
        const ImVec2 ss = bold->CalcTextSizeA(20 * scale, FLT_MAX, 0, star);
        dl->AddText(bold, 20 * scale, ImVec2(mark.x + (30 * scale - ss.x) / 2, mark.y + (30 * scale - ss.y) / 2), hex(0x173d51), star);
        dl->AddText(bold, 15.5f * scale, ImVec2(mark.x + 40 * scale, o.y + 11 * scale), hex(0xffffff), "SMS PC Port");
        dl->AddText(body, 12 * scale, ImVec2(mark.x + 40 * scale, o.y + 31 * scale), hex(0xc9e6ec),
                    "Closed \xC2\xB7 moving to SMS Launcher");
#ifdef _WIN32
        const char* platform = "Windows";
#else
        const char* platform = "Linux";
#endif
        const ImVec2 ps = body->CalcTextSizeA(13 * scale, FLT_MAX, 0, platform);
        dl->AddText(body, 13 * scale, ImVec2(o.x + W - padX - ps.x, o.y + (barH - ps.y) / 2), hex(0xc9e6ec), platform);

        // status bar
        const float sy = o.y + H - statusH;
        dl->AddRectFilled(ImVec2(o.x, sy), ImVec2(o.x + W, o.y + H), hex(0x082d45, 0xc7));
        dl->AddLine(ImVec2(o.x, sy), ImVec2(o.x + W, sy), hex(0xffffff, 0x30));
        const int ph = move.phase;
        dl->AddCircleFilled(ImVec2(o.x + padX + 4 * scale, sy + statusH / 2), 4 * scale,
                            ph == MoveJob::FAILED ? hex(0xff9a7a) : move.busy() ? hex(0xffca63) : hex(0x58d39d));
        const char* status = ph == MoveJob::DONE ? "SMS Launcher is ready" : move.busy() ? "Working\xE2\x80\xA6"
                           : ph == MoveJob::FAILED ? "Something went wrong" : "This launcher is closed";
        dl->AddText(body, 12 * scale, ImVec2(o.x + padX + 16 * scale, sy + (statusH - 12 * scale) / 2 - 1), hex(0xd2eaf0), status);
        const char* byo = "Bring your own ROM \xC2\xB7 No game data included";
        const ImVec2 bs = body->CalcTextSizeA(12 * scale, FLT_MAX, 0, byo);
        dl->AddText(body, 12 * scale, ImVec2(o.x + W - padX - bs.x, sy + (statusH - bs.y) / 2), hex(0xd2eaf0), byo);

        // intro, over the picture
        const float top = barH + 24 * scale;
        const char* eyebrow = "SUPER MARIO SUNSHINE";
        float ex = o.x + padX;
        for (const char* c = eyebrow; *c; c++) {  // letter-spaced, as .eyebrow
            const char s[2] = {*c, 0};
            dl->AddText(bold, 12 * scale, ImVec2(ex, o.y + top), hex(0xdcf3f7), s);
            ex += bold->CalcTextSizeA(12 * scale, FLT_MAX, 0, s).x + 1.6f * scale;
        }
        const float h1 = std::min(44.0f, std::max(30.0f, W / scale * 0.035f)) * scale;
        const char* title = "We've moved to SMS Launcher";
        dl->AddText(bold, h1, ImVec2(o.x + padX + 2 * scale, o.y + top + 22 * scale + 3 * scale), hex(0x082b45, 0x99), title);
        dl->AddText(bold, h1, ImVec2(o.x + padX, o.y + top + 22 * scale), hex(0xffffff), title);
        dl->AddText(body, 16 * scale, ImVec2(o.x + padX, o.y + top + 28 * scale + h1), hex(0xffffff),
                    "One download, then you're playing in the official launcher.");

        // the setup panel, centred
        const float panelW = std::min(W - padX * 2, 660.0f * scale);
        const float panelX = (W - panelW) / 2, panelY = top + 66 * scale + h1;
        ImGui::SetCursorPos(ImVec2(panelX, panelY));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0x072f49, 0xe8));
        ImGui::PushStyleColor(ImGuiCol_Border, hex(0xd3f2f5, 0x6e));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 21 * scale));
        ImGui::BeginChild("step", ImVec2(panelW, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0.0f);
        wrapped("This version is closed", hex(0xffffff), bold, 24 * scale);
        ImGui::Dummy(ImVec2(0, 2 * scale));
        wrapped("Its features are going into the official port and SMS Launcher by chasem-dev, who made the port. "
                "SMS Launcher sets the game up from your own disc image and keeps it up to date.",
                hex(0xd8eef1), body, 14 * scale);
        ImGui::Dummy(ImVec2(0, 10 * scale));
        // a .setup-file box: what happens to the saves, or where they went
        {
            const std::string text = !move.cardResult.empty() && ph != MoveJob::SAVES ? move.cardResult
                : "Backed up first, then put where SMS Launcher looks for them. Nothing here is deleted.";
            ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0xffffff, 0x12));
            ImGui::PushStyleColor(ImGuiCol_Border, hex(0xd3f2f5, 0x48));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 10 * scale));
            ImGui::BeginChild("saves", ImVec2(0, 0),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            wrapped("YOUR SAVES", hex(0xa9d8df), bold, 11 * scale);
            wrapped(text.c_str(), hex(0xffffff), bold, 14 * scale);
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
        if (ph == MoveJob::FAILED) {
            ImGui::Dummy(ImVec2(0, 6 * scale));
            wrapped(move.error.c_str(), hex(0xffb49a), body, 14 * scale);
        } else if (ph == MoveJob::DONE) {
            ImGui::Dummy(ImVec2(0, 6 * scale));
#ifdef _WIN32
            const std::string done = "SMS Launcher " + move.version + "'s installer is open. Finish it, then play from SMS Launcher.";
#else
            const std::string done = "SMS Launcher " + move.version + " is starting.";
#endif
            wrapped(done.c_str(), hex(0xa8eed5), bold, 14 * scale);
            wrapped(("Saved to " + move.file).c_str(), hex(0xd8eef1), body, 13 * scale);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        // the launch panel, at the bottom: the big button, or the progress
        const float launchW = std::min(W - padX * 2, 620.0f * scale), launchH = 104 * scale + (move.busy() ? 0 : 0);
        const float lx = o.x + (W - launchW) / 2, ly = sy - 16 * scale - launchH;
        // a rounded fill shaded across, as its 135-degree gradient
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + launchW, ly + launchH), hex(0xffffff, 0xf2), 12 * scale);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(lx, ly),
                                                      ImVec2(lx + launchW, ly + launchH), hex(0x123e59), hex(0x087e91));
        dl->AddRect(ImVec2(lx, ly), ImVec2(lx + launchW, ly + launchH), hex(0xa5e0e4, 0x7d), 12 * scale, 0, 1.0f);
        const float innerX = lx + 19 * scale, innerW = launchW - 38 * scale, btnH = 72 * scale, cog = 72 * scale;
        ImGui::SetCursorScreenPos(ImVec2(innerX, ly + 16 * scale));
        if (move.busy()) {
            char label[160];
            if (ph == MoveJob::SAVES) snprintf(label, sizeof label, "Backing up your saves\xE2\x80\xA6");
            else if (ph == MoveJob::FINDING) snprintf(label, sizeof label, "Finding the latest SMS Launcher\xE2\x80\xA6");
            else if (ph == MoveJob::DOWNLOADING) snprintf(label, sizeof label, "Downloading SMS Launcher %s", move.version.c_str());
            else snprintf(label, sizeof label, "Starting the installer\xE2\x80\xA6");
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddText(bold, 17 * scale, p, hex(0xffffff), label);
            if (ph == MoveJob::DOWNLOADING) {
                char pct[16];
                snprintf(pct, sizeof pct, "%.0f%%", double(move.progress()) * 100.0);
                const ImVec2 qs = bold->CalcTextSizeA(17 * scale, FLT_MAX, 0, pct);
                dl->AddText(bold, 17 * scale, ImVec2(p.x + innerW - cog - 12 * scale - qs.x, p.y), hex(0xffffff), pct);
            }
            // .progress-track
            const float ty = p.y + 34 * scale, tw = innerW - cog - 12 * scale;
            dl->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + tw, ty + 8 * scale), hex(0xffffff, 0x40), 4 * scale);
            if (ph == MoveJob::DOWNLOADING)
                dl->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + tw * move.progress(), ty + 8 * scale), hex(0xffca63), 4 * scale);
            else {  // indeterminate: a moving segment
                const float f = fmodf(t * 0.6f, 1.0f), seg = tw * 0.25f, x0 = p.x + (tw + seg) * f - seg;
                dl->AddRectFilled(ImVec2(std::max(p.x, x0), ty), ImVec2(std::min(p.x + tw, x0 + seg), ty + 8 * scale), hex(0xffca63), 4 * scale);
            }
            ImGui::SetCursorScreenPos(ImVec2(innerX + innerW - cog, ly + 16 * scale));
            if (smsButton("Cancel##move", ImVec2(cog, btnH), 1, 14 * scale,
                          ph == MoveJob::DOWNLOADING || ph == MoveJob::FINDING))
                move.stop();
        } else {
            const char* label = ph == MoveJob::DONE ? "Close" : ph == MoveJob::FAILED ? "Try again" : "Get SMS Launcher";
            if (smsButton(label, ImVec2(innerW - cog - 12 * scale, btnH), 0, 26 * scale)) {
                if (ph == MoveJob::DONE) quit = true;
                else {
                    move.cardFrom = cardDir();
                    move.begin();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(innerX + innerW - cog, ly + 16 * scale));
            // like the settings cog beside Play: a square for the release page
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            if (smsButton("##page", ImVec2(cog, btnH), 1, 14 * scale)) SDL_OpenURL(kSmsLauncherReleases);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open SMS Launcher's download page");
            const ImVec2 c(cp.x + cog / 2, cp.y + btnH / 2);  // an "open in browser" arrow
            const float a = 10 * scale;
            dl->AddRect(ImVec2(c.x - a, c.y - a + 3 * scale), ImVec2(c.x + a - 3 * scale, c.y + a), hex(0xffffff), 2 * scale, 0, 2 * scale);
            dl->AddLine(ImVec2(c.x - 1 * scale, c.y + 1 * scale), ImVec2(c.x + a + 1 * scale, c.y - a - 1 * scale), hex(0xffffff), 2 * scale);
            dl->AddLine(ImVec2(c.x + a + 1 * scale, c.y - a - 1 * scale), ImVec2(c.x + 3 * scale, c.y - a - 1 * scale), hex(0xffffff), 2 * scale);
            dl->AddLine(ImVec2(c.x + a + 1 * scale, c.y - a - 1 * scale), ImVec2(c.x + a + 1 * scale, c.y - 3 * scale), hex(0xffffff), 2 * scale);
        }
        (void)base;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !move.busy()) quit = true;
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return false;
    }

    // --- chrome
    void drawHeader(ImVec2 p0, ImVec2 p1, float t) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = p1.y - p0.y, w = p1.x - p0.x;
        const float seaY = p0.y + h * 0.70f;
        dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, seaY), kSky1, kSky1, kSky2, kSky2);
        // sun with slowly turning rays
        const ImVec2 sun(p1.x - w * 0.11f, p0.y + h * 0.40f);
        const float r = h * 0.22f;
        for (int i = 0; i < 12; i++) {
            float a = t * 0.15f + float(i) * 3.14159265f / 6.0f;
            dl->AddTriangleFilled(ImVec2(sun.x + cosf(a - 0.10f) * r * 1.25f, sun.y + sinf(a - 0.10f) * r * 1.25f),
                                  ImVec2(sun.x + cosf(a) * r * 2.0f, sun.y + sinf(a) * r * 2.0f),
                                  ImVec2(sun.x + cosf(a + 0.10f) * r * 1.25f, sun.y + sinf(a + 0.10f) * r * 1.25f),
                                  kSunGlow);
        }
        dl->AddCircleFilled(sun, r * 1.18f, kSunGlow, 48);
        dl->AddCircleFilled(sun, r, kSun, 48);
        // sea with moving waves
        dl->AddRectFilledMultiColor(ImVec2(p0.x, seaY), p1, kSea1, kSea1, kSea2, kSea2);
        for (int band = 0; band < 3; band++) {
            const float y = seaY + float(band) * h * 0.09f + h * 0.03f;
            const float amp = h * 0.012f * float(band + 1);
            ImVec2 pts[64];
            for (int i = 0; i < 64; i++) {
                float x = p0.x + w * float(i) / 63.0f;
                pts[i] = ImVec2(x, y + sinf(x * 0.02f / scale + t * (1.2f + band * 0.4f) + band) * amp);
            }
            dl->AddPolyline(pts, 64, IM_COL32(255, 255, 255, 70 - band * 18), 0, 2.0f * scale);
        }
        // title
        const float pad = 34.0f * scale;
        const char* title = "SUPER MARIO SUNSHINE";
        const float titleSize = 46.0f * scale;
        const ImVec2 tp(p0.x + pad, p0.y + h * 0.17f);
        dl->AddText(bold, titleSize, ImVec2(tp.x + 3 * scale, tp.y + 4 * scale), IM_COL32(10, 40, 90, 140), title);
        dl->AddText(bold, titleSize, tp, IM_COL32(255, 255, 255, 255), title);
        dl->AddText(bold, 20.0f * scale, ImVec2(tp.x + 2 * scale, tp.y + titleSize + 4 * scale),
                    IM_COL32(255, 225, 120, 255), "PC PORT   \xC2\xB7   LAUNCHER");
    }

    bool frame(bool& quit) {
        ImGuiIO& io = ImGui::GetIO();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float t = float(ImGui::GetTime());
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float W = vp->Size.x, H = vp->Size.y;
        const float headerH = std::min(170.0f * scale, H * 0.24f);
        drawHeader(origin, ImVec2(origin.x + W, origin.y + headerH), t);

        const float pad = 24.0f * scale, footerH = 76.0f * scale, sideW = 230.0f * scale;
        const float bodyY = headerH + pad, bodyH = H - headerH - footerH - pad * 1.5f;
        bool play = false;

        // sidebar
        ImGui::SetCursorPos(ImVec2(pad, bodyY));
        ImGui::BeginChild("nav", ImVec2(sideW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2(12 * scale, 14 * scale));
        ImGui::BeginGroup();
        ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.08f);
        // every page fits: items shrink from 46 when the window is short
        const float navH = std::min(46.0f * scale, (bodyH - 28.0f * scale) / float(P_COUNT) -
                                                       ImGui::GetStyle().ItemSpacing.y);
        for (int i = 0; i < P_COUNT; i++) {
            const bool on = page == i;
            ImGui::PushStyleColor(ImGuiCol_Header, on ? kAccent : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, on ? kAccentHot : ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(0.08f, 0.12f, 0.22f, 1.0f) : ImVec4(1, 1, 1, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * scale);
            char id[64];
            snprintf(id, sizeof id, "   %s##nav%d", kPageNames[i], i);
            if (ImGui::Selectable(id, true, 0, ImVec2(sideW - 24 * scale, navH))) {
                page = Page(i);
                capture = -1;
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
        }
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::EndChild();

        // page
        ImGui::SetCursorPos(ImVec2(pad * 2 + sideW, bodyY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 22 * scale));
        ImGui::BeginChild("page", ImVec2(W - sideW - pad * 3, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        drawBanner(quit);
        ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted(kPageNames[page]);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(kPageBlurbs[page]);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10 * scale));
        switch (page) {
        case P_INSTALL: pageInstall(); break;
        case P_CAMERA: pageCamera(); break;
        case P_DISPLAY: pageDisplay(); break;
        case P_GRAPHICS: pageGraphics(); break;
        case P_GAMEPLAY: pageGameplay(); break;
        case P_ONLINE: pageOnline(); break;
        case P_AUDIO: pageAudio(); break;
        case P_CONTROLS: pageControls(); break;
        default: pageAbout(quit); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // footer
        const float fy = H - footerH;
        ImGui::SetCursorPos(ImVec2(pad, fy + (footerH - 56 * scale) / 2));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (!status.empty() && ImGui::GetTime() < statusUntil) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(status.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted("Enter: play     Esc: quit     Arrows / controller: navigate");
        }
        ImGui::TextUnformatted("Settings are saved to settings.txt when you press Play.");
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        const float bw = 150 * scale, bh = 56 * scale, playW = 220 * scale;
        ImGui::SetCursorPos(ImVec2(W - pad - playW - (bw + 12 * scale) * 2, fy + (footerH - bh) / 2));
        if (ImGui::Button("Quit", ImVec2(bw, bh))) quit = true;
        ImGui::SameLine(0, 12 * scale);
        if (ImGui::Button("Save", ImVec2(bw, bh))) {
            const bool ok = settings.save() & bindings.save();
            status = ok ? "Settings saved." : "Could not write the settings files.";
            statusUntil = ImGui::GetTime() + 3.0;
        }
        ImGui::SameLine(0, 12 * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.12f, 0.22f, 1.0f));
        ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.35f);
        // the play button pulses gently
        const float pulse = 0.5f + 0.5f * sinf(t * 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
        if (ImGui::Button("PLAY  \xE2\x96\xB6", ImVec2(playW, bh))) play = true;
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin() - ImVec2(3, 3) * pulse * scale,
                                            ImGui::GetItemRectMax() + ImVec2(3, 3) * pulse * scale,
                                            IM_COL32(255, 214, 64, int(120 * (1.0f - pulse))), bh * 0.5f, 0,
                                            2.0f * scale);
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::PopStyleColor(4);

        if (capture < 0 && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                if (!ImGui::IsAnyItemActive()) play = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) quit = true;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) play = true;
        }
        if (play && (installed.empty() || job.running)) {  // nothing to play yet
            play = false;
            page = P_INSTALL;
            status = job.running ? "Wait for the install to finish." : "Install the game first: select your disc image.";
            statusUntil = ImGui::GetTime() + 5.0;
        }
        if (play && (tex.busy() || hd.busy())) {
            play = false;
            page = P_GRAPHICS;
            status = tex.busy() ? "Wait for the texture pack to finish, or cancel it."
                                : "Wait for the HD cutscenes to finish, or cancel them.";
            statusUntil = ImGui::GetTime() + 5.0;
        }
        ImGui::End();
        return play;
    }
};

bool shiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return (SDL_GetModState() & KMOD_SHIFT) != 0;
#endif
}

ImFont* loadFont(const char* const* paths, float size) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* const* p = paths; *p; p++) {
        FILE* f = fopen(*p, "rb");
        if (!f) continue;
        fclose(f);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(*p, size)) return font;
    }
    return nullptr;
}

}  // namespace

extern "C" int GXPC_RunLauncher(const char* settingsPath, const char* bindingsPath, int force) {
    Launcher L;
    L.settings.path = settingsPath && *settingsPath ? settingsPath : "settings.txt";
    L.bindings.path = bindingsPath && *bindingsPath ? bindingsPath : "bindings.txt";
    L.settings.load();
    {
        const std::string& p = L.settings.path;
        size_t slash = p.find_last_of("/\\");
        L.baseDir = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    }
    if (const char* src = getenv("SMS_LAUNCHER_DISC")) L.discSource = src;
    L.refreshInstalled();
    // `launcher = off` skips the menu, but never when there is no game to play
    const std::string show = L.settings.get("launcher", "on");
    (void)show;  // the move to SMS Launcher is shown even with the menu turned off
    if (L.installed.empty()) L.page = P_INSTALL;
    L.bindings.load();

#ifdef SDL_HINT_WINDOWS_DPI_SCALING
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[launcher] SDL_Init failed: %s; starting the game\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Rect usable = {0, 0, 1280, 800};
    SDL_GetDisplayUsableBounds(0, &usable);
    const int ww = std::min(1180, int(usable.w * 0.9f)), wh = std::min(800, int(usable.h * 0.9f));
    SDL_Window* win = SDL_CreateWindow("Super Mario Sunshine - Launcher", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) {
        fprintf(stderr, "[launcher] no OpenGL 3.3 window: %s; starting the game\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 900, 600);
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    // the few GL calls made here, outside ImGui's own loader
    typedef void(APIENTRY * ViewportFn)(GLint, GLint, GLsizei, GLsizei);
    typedef void(APIENTRY * ClearColorFn)(GLfloat, GLfloat, GLfloat, GLfloat);
    typedef void(APIENTRY * ClearFn)(GLbitfield);
    const ViewportFn glViewportP = (ViewportFn)SDL_GL_GetProcAddress("glViewport");
    const ClearColorFn glClearColorP = (ClearColorFn)SDL_GL_GetProcAddress("glClearColor");
    const ClearFn glClearP = (ClearFn)SDL_GL_GetProcAddress("glClear");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // SDL window units are already DPI-scaled (SDL_HINT_WINDOWS_DPI_SCALING);
    // the framebuffer scale makes fonts rasterize at full density.
    L.scale = 1.0f;
    applyTheme(L.scale);
    static const char* const kBody[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/System/Library/Fonts/Supplemental/Arial.ttf", nullptr};
    static const char* const kBold[] = {"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", nullptr};
    static const char* const kSymbols[] = {"C:\\Windows\\Fonts\\seguisym.ttf", nullptr};
    L.body = loadFont(kBody, 18.0f);
    if (!L.body) L.body = io.Fonts->AddFontDefault();
    L.bold = loadFont(kBold, 18.0f);
    if (!L.bold) L.bold = L.body;
    {  // the play arrow comes from a symbol font when the bold one lacks it
        ImFontConfig cfg;
        cfg.MergeMode = true;
        for (const char* const* p = kSymbols; *p; p++) {
            FILE* f = fopen(*p, "rb");
            if (!f) continue;
            fclose(f);
            io.Fonts->AddFontFromFileTTF(*p, 18.0f, &cfg);
            break;
        }
    }
    io.FontDefault = L.body;
    ImGui::GetStyle().FontSizeBase = 18.0f;

    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    L.scanDisplays();
    L.scanMods();
    {  // SMS Launcher's background picture, embedded at build time
        static const unsigned char kBg[] = {
#include "launcher_bg.inc"
        };
        int n = 0;
        if (unsigned char* px = stbi_load_from_memory(kBg, int(sizeof kBg), &L.bgW, &L.bgH, &n, 4)) {
            typedef void(APIENTRY * GenTexFn)(GLsizei, GLuint*);
            typedef void(APIENTRY * BindTexFn)(GLenum, GLuint);
            typedef void(APIENTRY * TexImageFn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
            typedef void(APIENTRY * TexParamFn)(GLenum, GLenum, GLint);
            const GenTexFn genTex = (GenTexFn)SDL_GL_GetProcAddress("glGenTextures");
            const BindTexFn bindTex = (BindTexFn)SDL_GL_GetProcAddress("glBindTexture");
            const TexImageFn texImage = (TexImageFn)SDL_GL_GetProcAddress("glTexImage2D");
            const TexParamFn texParam = (TexParamFn)SDL_GL_GetProcAddress("glTexParameteri");
            if (genTex && bindTex && texImage && texParam) {
                GLuint tex = 0;
                genTex(1, &tex);
                bindTex(GL_TEXTURE_2D, tex);
                texParam(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                texParam(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                texParam(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                texParam(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                texImage(GL_TEXTURE_2D, 0, GL_RGBA, L.bgW, L.bgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                L.bgTex = tex;
            }
            stbi_image_free(px);
        }
    }
    if (char* base = SDL_GetBasePath()) {
        L.exeDir = base;
        SDL_free(base);
    }
    UpdateJob::cleanup(L.exeDir);
    L.upd.exeDir = L.exeDir;
    {
        const std::string check = L.settings.get("update_check", "on");
        if (check != "off" && check != "0" && check != "no" && check != "false") L.startUpdateCheck();
    }

    bool play = false, quit = false;
    while (!play && !quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (L.capture >= 0 && ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                const std::string name = keyName(int(ev.key.keysym.scancode));
                std::string& keys = L.bindings.keys[L.capture];
                keys = L.captureAdd && !keys.empty() ? keys + " " + name : name;
                L.capture = -1;
                continue;  // the key that was bound does not also navigate
            }
            if (L.capture >= 0 && ev.type == SDL_KEYUP) continue;
            if ((ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) && ImGui::GetTime() < L.suppressClicks)
                continue;
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_DROPFILE) {  // a disc image dropped onto the window
                if (L.discSource.empty() && !L.job.running) {
                    snprintf(L.pickPath, sizeof L.pickPath, "%s", ev.drop.file);
                    L.page = P_INSTALL;
                }
                SDL_free(ev.drop.file);
            }
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
            if (ev.type == SDL_DISPLAYEVENT) {
                L.scanDisplays();
                L.modesFor = -1;
            }
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(30);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        play = L.movedFrame(quit);
        ImGui::Render();
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        glViewportP(0, 0, dw, dh);
        glClearColorP(0.035f, 0.10f, 0.20f, 1.0f);
        glClearP(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }
    if (play) {
        if (!L.settings.save()) fprintf(stderr, "[launcher] could not write %s\n", L.settings.path.c_str());
        if (!L.bindings.save()) fprintf(stderr, "[launcher] could not write %s\n", L.bindings.path.c_str());
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return play ? 1 : 0;
}

#else  // no SDL2: no launcher, the game starts directly
extern "C" int GXPC_RunLauncher(const char*, const char*, int) { return 1; }
#endif
