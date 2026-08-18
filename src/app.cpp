#include "app.h"

#include <shellapi.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <wil/resource.h>

#include "../res/resource.h"
#include "app_name.h"
#include "coords.h"
#include "log.h"
#include "tray.h"

namespace sc {
namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

UINT g_taskbarCreated = 0;  // RegisterWindowMessage("TaskbarCreated")
Tray g_tray;

// 메뉴 문자열을 리소스에서 읽는다. 실패하면 빈 문자열이 아니라 대체 문구를 쓴다.
void LoadMenuText(UINT id, const wchar_t* fallback, wchar_t* buffer, int count) {
    if (LoadStringW(GetModuleHandleW(nullptr), id, buffer, count) == 0) {
        wcscpy_s(buffer, static_cast<size_t>(count), fallback);
    }
}

void ShowTrayMenu(HWND hwnd, POINT screenPoint) {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return;
    }

    wchar_t text[128];

#if defined(_DEBUG)
    LoadMenuText(IDS_MENU_DUMP, L"좌표 로그 남기기", text, ARRAYSIZE(text));
    AppendMenuW(menu.get(), MF_STRING, IDM_DUMP_GEOMETRY, text);
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
#endif

    LoadMenuText(IDS_MENU_EXIT, L"종료", text, ARRAYSIZE(text));
    AppendMenuW(menu.get(), MF_STRING, IDM_EXIT, text);

    // 메뉴 밖을 눌러도 닫히게 하려면 먼저 포그라운드를 가져와야 한다.
    // 닫힌 뒤 WM_NULL을 던지는 것도 같은 이유의 오래된 처방이다.
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                     screenPoint.x, screenPoint.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void DumpGeometryAndOpenLog() {
    LogDesktopGeometry(L"트레이 메뉴 요청");
    const wchar_t* path = log::FilePath();
    if (path && path[0] != L'\0') {
        ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        g_tray.Restore();
        return 0;
    }

    switch (msg) {
        case kTrayCallbackMessage: {
            // NOTIFYICON_VERSION_4: wParam은 화면 좌표, lParam 하위 워드가 이벤트.
            const UINT event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU) {
                POINT pt{static_cast<LONG>(GET_X_LPARAM(wparam)),
                         static_cast<LONG>(GET_Y_LPARAM(wparam))};
                ShowTrayMenu(hwnd, pt);
            }
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
#if defined(_DEBUG)
                case IDM_DUMP_GEOMETRY:
                    DumpGeometryAndOpenLog();
                    return 0;
#endif
                default:
                    break;
            }
            break;

        // 계측: 모니터 구성이 바뀌면 좌표를 다시 찍는다.
        // 보조 모니터를 옮기거나 배율을 바꿨을 때 무엇이 달라졌는지 로그로 남는다.
        case WM_DISPLAYCHANGE:
            LogDesktopGeometry(L"WM_DISPLAYCHANGE");
            return 0;

        case WM_DPICHANGED:
            SC_LOG(L"WM_DPICHANGED: dpi=%u", LOWORD(wparam));
            LogDesktopGeometry(L"WM_DPICHANGED");
            return 0;

        case WM_DESTROY:
            g_tray.Remove();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// 트레이 상주 창.
//
// 메시지 전용 창(HWND_MESSAGE)을 쓰고 싶어지지만 그러면 안 된다.
// TaskbarCreated 같은 브로드캐스트는 최상위 창에만 전달된다.
// 그래서 진짜 최상위 창을 만들되 한 번도 보여 주지 않고, WS_EX_TOOLWINDOW로
// 작업 표시줄과 Alt+Tab에서 뺀다.
HWND CreateHostWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = SWEEPCAP_WNDCLASS_W;
    if (RegisterClassExW(&wc) == 0) {
        SC_LOG(L"RegisterClassEx 실패 err=%lu", GetLastError());
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, SWEEPCAP_WNDCLASS_W, SWEEPCAP_NAME_W,
                                WS_OVERLAPPED, 0, 0, 0, 0,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        SC_LOG(L"CreateWindowEx 실패 err=%lu", GetLastError());
    }
    return hwnd;
}

}  // namespace

int Run(HINSTANCE instance) {
    // 단일 인스턴스. 이미 떠 있으면 조용히 물러난다.
    // (빌드 후에도 옛 프로세스가 살아 있으면 변경이 반영돼 보이지 않는다.)
    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, SWEEPCAP_MUTEX_W)};
    const bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    log::Init();
    SC_LOG(L"%s %s 시작", SWEEPCAP_NAME_W, SWEEPCAP_VERSION_W);

    if (alreadyRunning) {
        SC_LOG(L"이미 실행 중이라 종료한다.");
        return 0;
    }

    // 매니페스트가 이미 Per-Monitor V2를 선언하지만, 실제로 무엇이 적용됐는지
    // 확인해서 로그로 남긴다. 매니페스트가 안 먹은 채 좌표가 틀어지는 상황을
    // 추측으로 쫓지 않기 위해서다.
    {
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(ctx);
        const bool isPerMonitorV2 =
            AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        SC_LOG(L"DPI 인식: awareness=%d, PerMonitorV2=%s", static_cast<int>(awareness),
               isPerMonitorV2 ? L"예" : L"아니오 (매니페스트 확인 필요)");
    }

    LogDesktopGeometry(L"시작");

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreated == 0) {
        SC_LOG(L"RegisterWindowMessage(TaskbarCreated) 실패 err=%lu", GetLastError());
    }

    HWND hwnd = CreateHostWindow(instance);
    if (!hwnd) {
        log::Shutdown();
        return 1;
    }

    if (!g_tray.Add(hwnd, kTrayCallbackMessage, kTrayIconId)) {
        // 트레이가 유일한 진입점이라 여기서 실패하면 사용자가 앱을 볼 방법이 없다.
        MessageBoxW(nullptr, L"트레이 아이콘을 등록하지 못했습니다.",
                    SWEEPCAP_NAME_W, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        log::Shutdown();
        return 1;
    }

    SC_LOG(L"트레이 상주 시작");

    MSG msg{};
    BOOL got = 0;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) {
            SC_LOG(L"GetMessage 실패 err=%lu", GetLastError());
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SC_LOG(L"종료 (코드 %d)", static_cast<int>(msg.wParam));
    log::Shutdown();
    return static_cast<int>(msg.wParam);
}

}  // namespace sc
