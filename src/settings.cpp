#include "settings.h"

#include <shlobj.h>
#include <wil/resource.h>

#include <atomic>
#include <cstdio>

#include "../res/resource.h"
#include "app_name.h"
#include "log.h"

namespace sc::settings {
namespace {

// The gestures offered in the tray menu.
//
// Shift is the snap key everywhere it is free. The one combination that claims
// it for the modifier gives snapping to Alt instead, which is why the pairing
// is stored per gesture rather than fixed: picking a modifier that contains
// the snap key would otherwise silently break snapped drags.
//
// Combinations known to be unusable are simply absent, so there is no way to
// select one. Alt+Shift and Ctrl+Shift are input-language switches on many
// systems, and a lone Shift or Alt collides with ordinary shell dragging.
constexpr Gesture kGestures[] = {
    {kCtrl | kAlt, kShift, IDS_GESTURE_CTRL_ALT},
    {kCtrl | kWin, kShift, IDS_GESTURE_CTRL_WIN},
    {kAlt | kWin, kShift, IDS_GESTURE_ALT_WIN},
    {kCtrl | kAlt | kWin, kShift, IDS_GESTURE_CTRL_ALT_WIN},
    {kCtrl | kShift | kWin, kAlt, IDS_GESTURE_CTRL_SHIFT_WIN},
};

// Ctrl+Win rather than Ctrl+Alt. Almost no application claims a Win-key
// combination, while Ctrl+Alt collides with application shortcuts and is what
// AltGr sends on many European layouts. The price is that Win combinations do
// not cross into windowed RDP sessions, which changing the gesture covers.
constexpr int kDefaultGestureIndex = 1;

// Pixel pitches first, then divisions. Index 2 (32 px) is the default.
//
// The division entries exist so a capture can be lined up with the same grid
// the window sizes use, and so a selection can reach the screen edge exactly.
constexpr GridChoice kGridChoices[] = {
    {16, 0, 0},  {24, 0, 0},   {32, 0, 0},   {48, 0, 0},   {64, 0, 0},
    {0, 12, 6},  {0, 24, 12},  {0, 48, 24},  {0, 96, 48},
};
constexpr int kDefaultGridIndex = 2;

// Divisions first here, the reverse of the capture list, because division is
// what a window size wants: a half, a third, a quarter and two thirds all have
// to land on a line, which needs a count divisible by 2, 3, 4 and 6.
//
// The default is 48 across rather than the 12 first chosen. Twelve satisfies
// the divisibility just as well but leaves cells of 320 by 360 on a 4K screen,
// which is far too coarse to size a window with: the first thing anyone did
// was go looking for something finer. Every count here divides the same way,
// so a finer default gives up nothing.
constexpr GridChoice kWindowGridChoices[] = {
    {0, 12, 6},  {0, 24, 12}, {0, 48, 24}, {0, 96, 48},
    {40, 0, 0},  {80, 0, 0},
};
constexpr int kDefaultWindowGridIndex = 2;  // 48 x 24

// Brightness kept outside the selection, in eighths. Eight is no dimming.
constexpr int kDimChoices[] = {8, 6, 5, 4};
constexpr int kDefaultDimIndex = 2;  // five eighths

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

int g_gestureIndex = kDefaultGestureIndex;
int g_gridIndex = kDefaultGridIndex;
int g_windowGridIndex = kDefaultWindowGridIndex;
// Read by the worker thread that builds the darkened copy, written only by the
// message loop. The only value here that crosses threads.
std::atomic<int> g_dimIndex{kDefaultDimIndex};
std::wstring g_captureRoot;      // resolved, never empty after Load
std::wstring g_configuredRoot;   // what the INI holds, empty when default
bool g_dateFolders = true;
std::wstring g_iniPath;

bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool MaskHeld(int mask) {
    if ((mask & kCtrl) != 0 && !KeyDown(VK_CONTROL)) {
        return false;
    }
    if ((mask & kAlt) != 0 && !KeyDown(VK_MENU)) {
        return false;
    }
    if ((mask & kShift) != 0 && !KeyDown(VK_SHIFT)) {
        return false;
    }
    if ((mask & kWin) != 0 && !KeyDown(VK_LWIN) && !KeyDown(VK_RWIN)) {
        return false;
    }
    return mask != 0;
}

// %LOCALAPPDATA%\SweepCap\sweepcap.ini, beside the debug log.
std::wstring ResolveIniPath() {
    wil::unique_cotaskmem_string base;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        return {};
    }
    std::wstring dir = base.get();
    dir += L'\\';
    dir += SWEEPCAP_NAME_W;
    CreateDirectoryW(dir.c_str(), nullptr);  // failing because it exists is fine
    return dir + L"\\sweepcap.ini";
}

void WriteInt(const wchar_t* section, const wchar_t* key, int value) {
    if (g_iniPath.empty()) {
        return;
    }
    wchar_t text[16];
    if (swprintf_s(text, L"%d", value) < 0) {
        return;
    }
    if (!WritePrivateProfileStringW(section, key, text, g_iniPath.c_str())) {
        SC_LOG(L"[settings] save failed %s/%s err=%lu", section, key, GetLastError());
    }
}

void WriteText(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    if (g_iniPath.empty()) {
        return;
    }
    // A null value deletes the key, which is how the default is expressed.
    if (!WritePrivateProfileStringW(section, key, value, g_iniPath.c_str())) {
        SC_LOG(L"[settings] save failed %s/%s err=%lu", section, key, GetLastError());
    }
}

void ApplyCaptureRoot(const wchar_t* configured) {
    g_configuredRoot = (configured != nullptr) ? configured : L"";
    g_captureRoot = g_configuredRoot.empty() ? DefaultCaptureRoot() : g_configuredRoot;
}

}  // namespace

void Load() {
    g_iniPath = ResolveIniPath();

    // The modifier mask is stored rather than the menu position, so inserting
    // a gesture into the list later cannot silently change what an existing
    // installation does.
    const int savedMask =
        g_iniPath.empty()
            ? 0
            : GetPrivateProfileIntW(L"gesture", L"modifiers", 0, g_iniPath.c_str());
    g_gestureIndex = kDefaultGestureIndex;
    for (size_t i = 0; i < ARRAYSIZE(kGestures); ++i) {
        if (kGestures[i].modifiers == savedMask) {
            g_gestureIndex = static_cast<int>(i);
            break;
        }
    }

    // The grid is stored as the value itself rather than a menu position, for
    // the same reason as the gesture: reordering the list later must not
    // change what an existing installation does.
    g_gridIndex = kDefaultGridIndex;
    if (!g_iniPath.empty()) {
        const int px = GetPrivateProfileIntW(L"capture", L"grid", 0, g_iniPath.c_str());
        const int cols = GetPrivateProfileIntW(L"capture", L"gridcols", 0, g_iniPath.c_str());
        const int rows = GetPrivateProfileIntW(L"capture", L"gridrows", 0, g_iniPath.c_str());
        for (size_t i = 0; i < ARRAYSIZE(kGridChoices); ++i) {
            const GridChoice& choice = kGridChoices[i];
            if (choice.px == px && choice.cols == cols && choice.rows == rows) {
                g_gridIndex = static_cast<int>(i);
                break;
            }
        }
    }

    g_windowGridIndex = kDefaultWindowGridIndex;
    if (!g_iniPath.empty()) {
        const int px = GetPrivateProfileIntW(L"window", L"grid", 0, g_iniPath.c_str());
        const int cols = GetPrivateProfileIntW(L"window", L"gridcols", 0, g_iniPath.c_str());
        const int rows = GetPrivateProfileIntW(L"window", L"gridrows", 0, g_iniPath.c_str());
        for (size_t i = 0; i < ARRAYSIZE(kWindowGridChoices); ++i) {
            const GridChoice& choice = kWindowGridChoices[i];
            if (choice.px == px && choice.cols == cols && choice.rows == rows) {
                g_windowGridIndex = static_cast<int>(i);
                break;
            }
        }
    }

    int dimIndex = kDefaultDimIndex;
    if (!g_iniPath.empty()) {
        const int keep = GetPrivateProfileIntW(L"capture", L"dim", 0, g_iniPath.c_str());
        for (size_t i = 0; i < ARRAYSIZE(kDimChoices); ++i) {
            if (kDimChoices[i] == keep) {
                dimIndex = static_cast<int>(i);
                break;
            }
        }
    }
    g_dimIndex.store(dimIndex, std::memory_order_relaxed);

    g_dateFolders = g_iniPath.empty() ||
                    GetPrivateProfileIntW(L"save", L"datefolders", 1, g_iniPath.c_str()) != 0;

    wchar_t folder[MAX_PATH] = L"";
    if (!g_iniPath.empty()) {
        GetPrivateProfileStringW(L"save", L"folder", L"", folder, ARRAYSIZE(folder),
                                 g_iniPath.c_str());
    }
    // A configured folder that is unreachable right now is kept anyway. Each
    // save tries it first and diverts to the default folder with a balloon
    // when it fails, so nothing is lost and nothing moves silently; and the
    // moment the folder comes back (a drive replugged), captures return to it
    // without a restart. Dropping it here instead would do both silently.
    if (folder[0] != L'\0' && GetFileAttributesW(folder) == INVALID_FILE_ATTRIBUTES) {
        SC_LOG(L"[settings] configured save folder is unreachable right now: %s", folder);
    }
    ApplyCaptureRoot(folder);

    const GridChoice& grid = kGridChoices[g_gridIndex];
    if (grid.ByDivision()) {
        SC_LOG(L"[settings] modifiers=%d grid=%dx%d divisions folder=%s%s",
               kGestures[g_gestureIndex].modifiers, grid.cols, grid.rows,
               g_captureRoot.c_str(), CaptureRootIsDefault() ? L" (default)" : L"");
    } else {
        SC_LOG(L"[settings] modifiers=%d grid=%dpx folder=%s%s", kGestures[g_gestureIndex].modifiers,
               grid.px, g_captureRoot.c_str(), CaptureRootIsDefault() ? L" (default)" : L"");
    }
}

const Gesture* Gestures(size_t* count) {
    if (count != nullptr) {
        *count = ARRAYSIZE(kGestures);
    }
    return kGestures;
}

int GestureIndex() {
    return g_gestureIndex;
}

void SetGestureIndex(int index) {
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kGestures))) {
        return;
    }
    g_gestureIndex = index;
    WriteInt(L"gesture", L"modifiers", kGestures[index].modifiers);
    SC_LOG(L"[settings] modifiers changed mask=%d snap=%d", kGestures[index].modifiers,
           kGestures[index].snap);
}

bool ModifiersHeld() {
    return MaskHeld(kGestures[g_gestureIndex].modifiers);
}

bool SnapModifierHeld() {
    return MaskHeld(kGestures[g_gestureIndex].snap);
}

const GridChoice* WindowGridChoices(size_t* count) {
    if (count != nullptr) {
        *count = ARRAYSIZE(kWindowGridChoices);
    }
    return kWindowGridChoices;
}

int WindowGridIndex() {
    return g_windowGridIndex;
}

const GridChoice& WindowGrid() {
    return kWindowGridChoices[g_windowGridIndex];
}

void SetWindowGridIndex(int index) {
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kWindowGridChoices))) {
        return;
    }
    g_windowGridIndex = index;
    const GridChoice& choice = kWindowGridChoices[index];
    WriteInt(L"window", L"grid", choice.px);
    WriteInt(L"window", L"gridcols", choice.cols);
    WriteInt(L"window", L"gridrows", choice.rows);
    if (choice.ByDivision()) {
        SC_LOG(L"[settings] window grid changed %d x %d divisions", choice.cols, choice.rows);
    } else {
        SC_LOG(L"[settings] window grid changed %dpx", choice.px);
    }
}

const int* DimChoices(size_t* count) {
    if (count != nullptr) {
        *count = ARRAYSIZE(kDimChoices);
    }
    return kDimChoices;
}

int DimIndex() {
    return g_dimIndex.load(std::memory_order_relaxed);
}

int DimKeepEighths() {
    return kDimChoices[g_dimIndex.load(std::memory_order_relaxed)];
}

void SetDimIndex(int index) {
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kDimChoices))) {
        return;
    }
    g_dimIndex.store(index, std::memory_order_relaxed);
    WriteInt(L"capture", L"dim", kDimChoices[index]);
    SC_LOG(L"[settings] outside brightness changed %d/8", kDimChoices[index]);
}

const GridChoice* GridChoices(size_t* count) {
    if (count != nullptr) {
        *count = ARRAYSIZE(kGridChoices);
    }
    return kGridChoices;
}

int GridIndex() {
    return g_gridIndex;
}

const GridChoice& Grid() {
    return kGridChoices[g_gridIndex];
}

void SetGridIndex(int index) {
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kGridChoices))) {
        return;
    }
    g_gridIndex = index;
    const GridChoice& choice = kGridChoices[index];
    WriteInt(L"capture", L"grid", choice.px);
    WriteInt(L"capture", L"gridcols", choice.cols);
    WriteInt(L"capture", L"gridrows", choice.rows);
    if (choice.ByDivision()) {
        SC_LOG(L"[settings] grid changed %d x %d divisions", choice.cols, choice.rows);
    } else {
        SC_LOG(L"[settings] grid changed %dpx", choice.px);
    }
}

const std::wstring& CaptureRoot() {
    return g_captureRoot;
}

bool CaptureRootIsDefault() {
    return g_configuredRoot.empty();
}

std::wstring DefaultCaptureRoot() {
    wil::unique_cotaskmem_string pictures;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) {
        return {};
    }
    std::wstring root = pictures.get();
    root += L'\\';
    root += SWEEPCAP_NAME_W;
    return root;
}

void SetCaptureRoot(const wchar_t* path) {
    const bool toDefault = (path == nullptr || path[0] == L'\0');
    ApplyCaptureRoot(toDefault ? nullptr : path);
    WriteText(L"save", L"folder", toDefault ? nullptr : path);
    SC_LOG(L"[settings] save folder changed %s%s", g_captureRoot.c_str(),
           toDefault ? L" (default)" : L"");
}

bool DateFolders() {
    return g_dateFolders;
}

void SetDateFolders(bool on) {
    if (g_dateFolders == on) {
        return;
    }
    g_dateFolders = on;
    WriteInt(L"save", L"datefolders", on ? 1 : 0);
    SC_LOG(L"[settings] date subfolders %s", on ? L"on" : L"off");
}

bool RunAtStartup() {
    wil::unique_hkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    return RegQueryValueExW(key.get(), SWEEPCAP_NAME_W, nullptr, nullptr, nullptr,
                            nullptr) == ERROR_SUCCESS;
}

bool SetRunAtStartup(bool on) {
    wil::unique_hkey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        SC_LOG(L"[settings] could not open the Run key err=%lu", GetLastError());
        return false;
    }

    if (!on) {
        const LSTATUS status = RegDeleteValueW(key.get(), SWEEPCAP_NAME_W);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (length == 0 || length >= ARRAYSIZE(exe)) {
        return false;
    }
    // Quoted, so a path containing spaces is not read as a command plus
    // arguments.
    wchar_t quoted[MAX_PATH + 4];
    if (swprintf_s(quoted, L"\"%s\"", exe) < 0) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(key.get(), SWEEPCAP_NAME_W, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(quoted), bytes);
    if (status != ERROR_SUCCESS) {
        SC_LOG(L"[settings] could not write the Run value status=%ld", status);
        return false;
    }
    return true;
}

}  // namespace sc::settings
