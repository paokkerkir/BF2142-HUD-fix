// BF2142 widescreen HUD fix - port of marekzajac97/BF2-HUD-fix to BF2142 1.51.
// Client-side only: squeezes the 800x600 HUD into a 4:3 area and moves the
// side elements out to the screen edges by intercepting hudBuilder commands
// as the (server-validated, untouched) HUD scripts run.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <fstream>

#include "GameString.h"

static_assert(sizeof(void*) == 4, "x86 only");

#define CONFIG_NAME L"hudfix2142.ini"
#define LOG_NAME    L"hudfix2142.log"

// ---------------------------------------------------------------------------
// BF2142.exe 1.51 addresses (image base 0x400000)

#define G_RENDERER             0xA2020C // dice::hfe::IRenderer*

// hudBuilder console-command wrappers: call sites into the builder methods
#define SITE_CREATE_SPLIT      0x764106
#define SITE_CREATE_TRANSFORM  0x763E3C
#define SITE_CREATE_PICTURE    0x7681A0
#define SITE_CREATE_TEXT       0x76CEDD
#define SITE_CREATE_BUTTON     0x76B926
#define SITE_SET_MINI_POS      0x76125F
#define SITE_HUD_CAMERA_SETUP  0x7CA6E9

#define FN_CREATE_SPLIT        0x7B2AA0 // bool HudBuilder::createSplitNode(string parent, string name)
#define FN_CREATE_TRANSFORM    0x7B29E0 // bool HudBuilder::create*Node(string parent, string name, int x, int y, int w, int h)
#define FN_CREATE_PICTURE      0x7B3480
#define FN_CREATE_TEXT         0x7B35D0
#define FN_CREATE_BUTTON       0x7B38A0
#define FN_SET_MINI_POS        0x761274 // bool MapNode::setMiniPos(float x, float y)
#define FN_SET_ACTIVE_OBJECT   0x7ADAF0 // bool HudBuilder::setActiveObject(string parent, string name)
#define FN_HUD_CAMERA_SETUP    0x7C94B0 // sets up the 800x600 HUD ortho camera

#define IMM_HUD_WIDTH          0x7C94E9 // push 800.0f  -> HUD camera width
#define IMM_MOUSE_AREA_WIDTH   0x753EC6 // mov [ebp-4], 800.0f  -> mouse area width
#define IMM_MOUSE_AREA_OFFSET  0x753F04 // mov [ebp+0Ch], -400.0f -> mouse area x origin
#define JMP_CROSSHAIR          0x7BE845 // jle -> jmp, stops crosshair jitter when going prone

// RendDX9_ori.dll / RendDX9.dll 1.51
#define RVA_SET_FOV_SAFE       0x66AC0  // RenderView::setFovSafe(float)
#define RVA_SET_FOV_SAFE_SLOT  0x1C6628 // its vtable slot

struct SiteCheck { DWORD va; BYTE bytes[5]; };
static const SiteCheck g_sites[] = {
    { SITE_CREATE_SPLIT,     { 0xE8, 0x95, 0xE9, 0x04, 0x00 } },
    { SITE_CREATE_TRANSFORM, { 0xE8, 0x9F, 0xEB, 0x04, 0x00 } },
    { SITE_CREATE_PICTURE,   { 0xE8, 0xDB, 0xB2, 0x04, 0x00 } },
    { SITE_CREATE_TEXT,      { 0xE8, 0xEE, 0x66, 0x04, 0x00 } },
    { SITE_CREATE_BUTTON,    { 0xE8, 0x75, 0x7F, 0x04, 0x00 } },
    { SITE_SET_MINI_POS,     { 0xE8, 0x10, 0x00, 0x00, 0x00 } },
    { SITE_HUD_CAMERA_SETUP, { 0xE8, 0xC2, 0xED, 0xFF, 0xFF } },
    { IMM_HUD_WIDTH,         { 0x00, 0x00, 0x48, 0x44, 0xFF } },
    { IMM_MOUSE_AREA_WIDTH,  { 0x00, 0x00, 0x48, 0x44, 0xC7 } },
    { IMM_MOUSE_AREA_OFFSET, { 0x00, 0x00, 0xC8, 0xC3, 0xC7 } },
    { JMP_CROSSHAIR,         { 0x7E, 0x3A, 0xE8, 0x48, 0xAE } },
    { FN_SET_ACTIVE_OBJECT,  { 0x55, 0x8B, 0xEC, 0x83, 0xEC } },
};

// ---------------------------------------------------------------------------
// config

struct Rect { int x, y, w, h; };

bool g_logging = true;
bool g_offsetMinimap = true;
bool g_fixCrosshair = true;
float g_fovScale = 1.0f;
int g_forcedWidth = 0, g_forcedHeight = 0;
std::set<std::string> g_nodesRight;
std::set<std::string> g_nodesLeft;
std::set<std::string> g_nodesFillRight;
std::set<std::string> g_nodesFillLeft;
std::set<std::string> g_nodesMakeMovable;
std::set<std::string> g_nodesKeepStretched;
std::map<std::string, std::vector<std::string>> g_nodesClone;
std::map<std::string, std::string> g_nodesOverrideParent;
std::map<std::string, Rect> g_nodesRect;

// state
float g_aspect = 4.0f / 3.0f;
int g_hudOffset = 0;   // HUD units (of 800) to push edge elements outward
int g_resW = 0, g_resH = 0;
bool g_fovPatched = false;

wchar_t g_dir[MAX_PATH];
FILE* g_log = nullptr;

void log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list va;
    va_start(va, fmt);
    vfprintf(g_log, fmt, va);
    va_end(va);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// memory helpers

bool writeMem(DWORD dest, const void* src, size_t len) {
    DWORD old;
    if (!VirtualProtect((void*)dest, len, PAGE_EXECUTE_READWRITE, &old)) {
        log("ERROR: VirtualProtect(0x%08X) failed: %u", dest, GetLastError());
        return false;
    }
    memcpy((void*)dest, src, len);
    VirtualProtect((void*)dest, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)dest, len);
    return true;
}

bool hookCall(DWORD site, void* fn) {
    BYTE call[5] = { 0xE8 };
    *(DWORD*)(call + 1) = (DWORD)fn - site - 5;
    return writeMem(site, call, 5);
}

void writeFloat(DWORD dest, float v) { writeMem(dest, &v, 4); }

// ---------------------------------------------------------------------------
// resolution / aspect

struct WndSearch { DWORD pid; HWND best; LONG area; };

BOOL CALLBACK enumWnd(HWND hwnd, LPARAM lp) {
    WndSearch* s = (WndSearch*)lp;
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rc;
    if (GetClientRect(hwnd, &rc) && rc.right * rc.bottom > s->area) {
        s->area = rc.right * rc.bottom;
        s->best = hwnd;
    }
    return TRUE;
}

bool sane(int w, int h) {
    return w >= 640 && h >= 480 && w <= 16384 && h <= 16384 && (float)w / h > 0.5f && (float)w / h < 6.0f;
}

bool getResolution(int& w, int& h) {
    if (g_forcedWidth > 0 && g_forcedHeight > 0) {
        w = g_forcedWidth; h = g_forcedHeight;
        return true;
    }

    int rw = 0, rh = 0;
    BYTE* renderer = *(BYTE**)G_RENDERER;
    if (renderer && !IsBadReadPtr(renderer + 0x24, 8)) {
        rw = *(int*)(renderer + 0x24);
        rh = *(int*)(renderer + 0x28);
    }
    if (sane(rw, rh) && rw == g_resW && rh == g_resH) { // unchanged: skip the window search
        w = rw; h = rh;
        return true;
    }

    int ww = 0, wh = 0;
    WndSearch s = { GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(enumWnd, (LPARAM)&s);
    if (s.best) {
        RECT rc;
        GetClientRect(s.best, &rc);
        ww = rc.right; wh = rc.bottom;
    }

    // the renderer is authoritative when it looks valid; the window is a fallback
    // (a DPI-virtualized window still has the right aspect ratio)
    if (sane(rw, rh)) { w = rw; h = rh; }
    else if (sane(ww, wh)) { w = ww; h = wh; }
    else return false;

    if (w != g_resW || h != g_resH)
        log("Resolution: renderer %dx%d, window %dx%d -> using %dx%d", rw, rh, ww, wh, w, h);
    return true;
}

// recomputes the HUD squeeze; cheap enough to run every time the HUD camera is set up
void refreshAspect() {
    int w, h;
    if (!getResolution(w, h) || (w == g_resW && h == g_resH))
        return;
    g_resW = w; g_resH = h;
    g_aspect = (float)w / (float)h;

    float hudWidth = 600.0f * g_aspect;
    if (g_aspect <= 4.0f / 3.0f + 0.001f) hudWidth = 800.0f; // 4:3 or taller: nothing to fix
    g_hudOffset = (int)((hudWidth - 800.0f) / 2);

    log("Aspect %.4f: HUD width %.1f, edge offset %d", g_aspect, hudWidth, g_hudOffset);
    writeFloat(IMM_HUD_WIDTH, hudWidth);         // squeeze HUD into the centre 4:3 area
    writeFloat(IMM_MOUSE_AREA_WIDTH, hudWidth);  // let the cursor reach the screen edges again
    writeFloat(IMM_MOUSE_AREA_OFFSET, -hudWidth / 2);
}

// ---------------------------------------------------------------------------
// node adjustments, applied as each node is created

void adjustNode(const std::string& name, int& x, int& y, int& w, int& h) {
    auto r = g_nodesRect.find(name);
    if (r != g_nodesRect.end()) {
        x = r->second.x; y = r->second.y; w = r->second.w; h = r->second.h;
    }

    refreshAspect();
    int off = g_hudOffset;
    if (g_nodesLeft.count(name))  x -= off;
    if (g_nodesRight.count(name)) x += off;
    if (g_nodesFillRight.count(name)) w = off;
    if (g_nodesFillLeft.count(name)) { w = off; x -= off; }
    if (g_nodesKeepStretched.count(name)) {
        w = (int)(w * g_aspect / (4.0f / 3.0f));
        x -= off;
    }
}

std::string parentFor(const std::string& node, const char* parent) {
    auto it = g_nodesOverrideParent.find(node);
    if (it == g_nodesOverrideParent.end())
        return parent;
    log("Overriding parent for node %s from %s to %s", node.c_str(), parent, it->second.c_str());
    return it->second;
}

typedef bool(__thiscall* CreateRectNode_t)(void*, game::string, game::string, int, int, int, int);
typedef bool(__thiscall* CreateSplitNode_t)(void*, game::string, game::string);
typedef bool(__thiscall* SetActiveObject_t)(void*, game::string, game::string);
typedef bool(__thiscall* SetMiniPos_t)(void*, float, float);
typedef void(__thiscall* HudCameraSetup_t)(void*);
typedef void(__thiscall* SetFov_t)(void*, float);

void logAdjusted(const char* kind, const std::string& name, int x, int y, int w, int h, int ox, int oy, int ow, int oh) {
    if (x != ox || y != oy || w != ow || h != oh)
        log("%s %s: %d %d %d %d -> %d %d %d %d", kind, name.c_str(), ox, oy, ow, oh, x, y, w, h);
}

bool createTransform(void* builder, const game::string& parent, const game::string& node, int x, int y, int w, int h) {
    std::string name = node.c_str();
    std::string par = parentFor(name, parent.c_str());
    CreateRectNode_t orig = (CreateRectNode_t)FN_CREATE_TRANSFORM;

    int ox = x, oy = y, ow = w, oh = h;
    adjustNode(name, x, y, w, h);
    logAdjusted("TransformNode", name, x, y, w, h, ox, oy, ow, oh);
    bool res = orig(builder, game::string(par.c_str()), node, x, y, w, h);

    // clones come after the original so they can be parented to it (inheriting
    // its show variables); then the original is made the active object again,
    // because the script's following setNode* lines are meant for it
    auto clones = g_nodesClone.find(name);
    if (clones != g_nodesClone.end()) {
        for (const std::string& clone : clones->second) {
            int cx = ox, cy = oy, cw = ow, ch = oh;
            adjustNode(clone, cx, cy, cw, ch);
            std::string cpar = parentFor(clone, par.c_str());
            log("Creating TransformNode %s %s %d %d %d %d (clone of %s)", cpar.c_str(), clone.c_str(), cx, cy, cw, ch, name.c_str());
            orig(builder, game::string(cpar.c_str()), game::string(clone.c_str()), cx, cy, cw, ch);
        }
        ((SetActiveObject_t)FN_SET_ACTIVE_OBJECT)(builder, game::string(par.c_str()), node);
    }
    return res;
}

bool __fastcall CreateTransformNode_hook(void* builder, int, game::string parent, game::string node, int x, int y, int w, int h) {
    return createTransform(builder, parent, node, x, y, w, h);
}

bool __fastcall CreateSplitNode_hook(void* builder, int, game::string parent, game::string node) {
    std::string name = node.c_str();
    if (g_nodesMakeMovable.count(name)) {
        log("Creating %s as TransformNode instead of SplitNode", name.c_str());
        return createTransform(builder, parent, node, 0, 0, 800, 600);
    }
    std::string par = parentFor(name, parent.c_str());
    return ((CreateSplitNode_t)FN_CREATE_SPLIT)(builder, game::string(par.c_str()), node);
}

// picture/text/button nodes: parent override + rect/offset adjustments
template <DWORD ORIG>
bool __fastcall CreateRectNode_hook(void* builder, int, game::string parent, game::string node, int x, int y, int w, int h) {
    std::string name = node.c_str();
    std::string par = parentFor(name, parent.c_str());
    int ox = x, oy = y, ow = w, oh = h;
    adjustNode(name, x, y, w, h);
    logAdjusted("Node", name, x, y, w, h, ox, oy, ow, oh);
    return ((CreateRectNode_t)ORIG)(builder, game::string(par.c_str()), node, x, y, w, h);
}

bool __fastcall SetMiniPos_hook(void* mapNode, int, float x, float y) {
    if (g_offsetMinimap) {
        refreshAspect();
        log("Minimap position %.0f/%.0f -> %.0f/%.0f", x, y, x + g_hudOffset, y);
        x += g_hudOffset;
    }
    return ((SetMiniPos_t)FN_SET_MINI_POS)(mapNode, x, y);
}

// ---------------------------------------------------------------------------
// optional FOV scale. Unlike BF2, 2142's renderer never clamps wide aspect
// ratios (its 16:9 limit constant is 0.05625 instead of 0.5625), so
// ultrawide already gets the full horizontal FOV and no fix is needed.

SetFov_t g_origSetFovSafe = nullptr;

void __fastcall SetFovSafe_hook(void* view, int, float fov) {
    g_origSetFovSafe(view, fov * g_fovScale);
}

void patchFov() {
    g_fovPatched = true;
    if (g_fovScale == 1.0f) return;

    static const BYTE expect[] = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x34, 0xD8, 0x1D };
    const char* names[] = { "RendDX9_ori.dll", "RendDX9.dll" }; // OpenSpy installs a forwarding RendDX9.dll
    for (const char* n : names) {
        DWORD base = (DWORD)GetModuleHandleA(n);
        if (!base) continue;
        DWORD fn = base + RVA_SET_FOV_SAFE, slot = base + RVA_SET_FOV_SAFE_SLOT;
        if (IsBadReadPtr((void*)fn, sizeof(expect)) || memcmp((void*)fn, expect, sizeof(expect)) || *(DWORD*)slot != fn)
            continue;
        g_origSetFovSafe = (SetFov_t)fn;
        void* hook = (void*)SetFovSafe_hook;
        writeMem(slot, &hook, 4);
        log("FOV scale %.3f applied via %s", g_fovScale, n);
        return;
    }
    log("WARNING: unknown RendDX9 build, fov_scale ignored");
}

void __fastcall HudCameraSetup_hook(void* self) {
    refreshAspect();
    if (!g_fovPatched) patchFov();
    ((HudCameraSetup_t)FN_HUD_CAMERA_SETUP)(self);
}

// ---------------------------------------------------------------------------

void readConfig() {
    std::wstring path = std::wstring(g_dir) + CONFIG_NAME;
    std::ifstream config(path);
    if (!config.is_open()) {
        log("WARNING: %ls not found, only the HUD squeeze is applied", path.c_str());
        return;
    }

    std::string line;
    while (std::getline(config, line)) {
        line = line.substr(0, line.find('#'));
        std::vector<std::string> a;
        std::istringstream iss(line);
        for (std::string t; iss >> t; )
            a.push_back(t);
        if (a.empty()) continue;

        const std::string& k = a[0];
        try {
            if (k == "offset_right" && a.size() == 2)          g_nodesRight.insert(a[1]);
            else if (k == "offset_left" && a.size() == 2)      g_nodesLeft.insert(a[1]);
            else if (k == "fill_left" && a.size() == 2)        g_nodesFillLeft.insert(a[1]);
            else if (k == "fill_right" && a.size() == 2)       g_nodesFillRight.insert(a[1]);
            else if (k == "make_movable" && a.size() == 2)     g_nodesMakeMovable.insert(a[1]);
            else if (k == "keep_stretched" && a.size() == 2)   g_nodesKeepStretched.insert(a[1]);
            else if (k == "clone" && a.size() == 3)            g_nodesClone[a[1]].push_back(a[2]);
            else if (k == "override_parent" && a.size() == 3)  g_nodesOverrideParent[a[1]] = a[2];
            else if (k == "rect" && a.size() == 6)
                g_nodesRect[a[1]] = { std::stoi(a[2]), std::stoi(a[3]), std::stoi(a[4]), std::stoi(a[5]) };
            else if (k == "hide" && a.size() == 2)             g_nodesRect[a[1]] = { 0, 0, 0, 0 };
            else if (k == "offset_minimap" && a.size() == 2)   g_offsetMinimap = std::stoi(a[1]) != 0;
            else if (k == "fix_crosshair" && a.size() == 2)    g_fixCrosshair = std::stoi(a[1]) != 0;
            else if (k == "fov_scale" && a.size() == 2)        g_fovScale = std::stof(a[1]);
            else if (k == "resolution" && a.size() == 3) {
                g_forcedWidth = std::stoi(a[1]);
                g_forcedHeight = std::stoi(a[2]);
            }
            else if (k == "log" && a.size() == 2)              ; // handled before the log opens
            else log("WARNING: bad config line: %s", line.c_str());
        }
        catch (...) {
            log("WARNING: bad number in config line: %s", line.c_str());
        }
    }
}

bool readLogSetting() {
    std::ifstream config(std::wstring(g_dir) + CONFIG_NAME);
    std::string line;
    while (std::getline(config, line)) {
        std::istringstream iss(line.substr(0, line.find('#')));
        std::string k, v;
        if (iss >> k >> v && k == "log")
            return v != "0";
    }
    return true;
}

bool install() {
    for (const SiteCheck& s : g_sites) {
        if (memcmp((void*)s.va, s.bytes, sizeof(s.bytes)) != 0) {
            log("ERROR: unexpected code at 0x%08X - not BF2142.exe 1.51, hudfix disabled", s.va);
            return false;
        }
    }

    hookCall(SITE_CREATE_SPLIT, CreateSplitNode_hook);
    hookCall(SITE_CREATE_TRANSFORM, CreateTransformNode_hook);
    hookCall(SITE_CREATE_PICTURE, CreateRectNode_hook<FN_CREATE_PICTURE>);
    hookCall(SITE_CREATE_TEXT, CreateRectNode_hook<FN_CREATE_TEXT>);
    hookCall(SITE_CREATE_BUTTON, CreateRectNode_hook<FN_CREATE_BUTTON>);
    hookCall(SITE_SET_MINI_POS, SetMiniPos_hook);
    hookCall(SITE_HUD_CAMERA_SETUP, HudCameraSetup_hook);

    if (g_fixCrosshair) {
        BYTE jmp = 0xEB;
        writeMem(JMP_CROSSHAIR, &jmp, 1);
    }
    log("Hooks installed");
    return true;
}

// ---------------------------------------------------------------------------
// dinput8.dll proxy, so no ASI loader is needed (the DLL also works as an .asi)

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    static DirectInput8Create_t real = nullptr;
    if (!real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        HMODULE h = LoadLibraryW(path);
        if (h) real = (DirectInput8Create_t)GetProcAddress(h, "DirectInput8Create");
        if (!real) return E_FAIL;
    }
    return real(inst, ver, riid, out, outer);
}
#pragma comment(linker, "/EXPORT:DirectInput8Create=_Proxy_DirectInput8Create@20")

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(hModule);

    // only patch BF2142.exe, and only once if loaded both as dinput8.dll and .asi
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;
    if (_wcsicmp(exeName, L"BF2142.exe") != 0)
        return TRUE;
    wchar_t mutexName[64];
    swprintf_s(mutexName, L"bf2142hudfix_%u", GetCurrentProcessId());
    CreateMutexW(nullptr, FALSE, mutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return TRUE;

    GetModuleFileNameW(hModule, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) slash[1] = 0;

    if (readLogSetting())
        g_log = _wfopen((std::wstring(g_dir) + LOG_NAME).c_str(), L"w");
    log("BF2142 widescreen HUD fix v1.0");

    readConfig();
    install();
    return TRUE;
}
