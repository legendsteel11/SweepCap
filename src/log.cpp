#include "log.h"

#include <psapi.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

#include "app_name.h"

namespace sc::log {
namespace {

std::mutex g_mutex;
wil::unique_hfile g_file;
wchar_t g_path[MAX_PATH] = L"";
bool g_initialized = false;

// Builds %LOCALAPPDATA%\SweepCap\sweepcap-debug.log and makes sure the folder exists.
bool ResolvePath() {
    wil::unique_cotaskmem_string base;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        return false;
    }
    wchar_t dir[MAX_PATH];
    if (swprintf_s(dir, L"%s\\%s", base.get(), SWEEPCAP_NAME_W) < 0) {
        return false;
    }
    CreateDirectoryW(dir, nullptr);  // failing because it already exists is fine
    return swprintf_s(g_path, L"%s\\sweepcap-debug.log", dir) >= 0;
}

}  // namespace

void Init() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        return;
    }
    g_initialized = true;

    if (!ResolvePath()) {
        return;
    }
    // Start fresh on every run so the instrumentation does not slowly eat disk.
    g_file.reset(CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!g_file) {
        return;
    }
    // UTF-8 BOM, so Notepad and editors read the Korean text correctly.
    static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(g_file.get(), kBom, sizeof(kBom), &written, nullptr);
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    g_file.reset();
    g_initialized = false;
}

const wchar_t* FilePath() { return g_path; }

void WriteResourceUsage(const wchar_t* tag) {
    const HANDLE self = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    const bool haveMemory = GetProcessMemoryInfo(self, &memory, sizeof(memory)) != FALSE;

    // GR_GDIOBJECTS and GR_USEROBJECTS are per-process totals. A capture
    // creates and destroys several of each, so a healthy run returns to the
    // same numbers rather than to zero.
    Write(L"[자원] %s GDI=%u USER=%u 핸들=%lu 작업세트=%.1f MB", tag,
          GetGuiResources(self, GR_GDIOBJECTS), GetGuiResources(self, GR_USEROBJECTS),
          [] {
              DWORD count = 0;
              GetProcessHandleCount(GetCurrentProcess(), &count);
              return count;
          }(),
          haveMemory ? static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0) : 0.0);
}

void Write(const wchar_t* fmt, ...) {
    wchar_t body[2048];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(body, _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0 && body[0] == L'\0') {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t line[2200];
    if (swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute,
                   st.wSecond, st.wMilliseconds, body) < 0) {
        return;
    }

    OutputDebugStringW(line);

    std::lock_guard lock(g_mutex);
    if (!g_file) {
        return;
    }
    // The file holds UTF-8.
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return;
    }
    std::string utf8(static_cast<size_t>(bytes) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(g_file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

}  // namespace sc::log
