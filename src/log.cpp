#include "log.h"

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

// %LOCALAPPDATA%\SweepCap\sweepcap-debug.log 경로를 만들고 폴더를 확보한다.
bool ResolvePath() {
    wil::unique_cotaskmem_string base;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        return false;
    }
    wchar_t dir[MAX_PATH];
    if (swprintf_s(dir, L"%s\\%s", base.get(), SWEEPCAP_NAME_W) < 0) {
        return false;
    }
    CreateDirectoryW(dir, nullptr);  // 이미 있으면 실패해도 상관없다.
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
    // 실행할 때마다 새로 쓴다. 계측기가 디스크를 야금야금 먹지 않게 한다.
    g_file.reset(CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!g_file) {
        return;
    }
    // UTF-8 BOM. 메모장과 에디터가 한글을 제대로 읽게 한다.
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
    // 파일에는 UTF-8로 남긴다.
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
