#include "app.h"

#include <shellapi.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <wil/resource.h>

#include "../res/resource.h"
#include "app_name.h"
#include "config.h"
#include "coords.h"
#include "hook.h"
#include "log.h"
#include "session.h"
#include "tray.h"

namespace sc {
namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

UINT g_taskbarCreated = 0;  // RegisterWindowMessage("TaskbarCreated")
Tray g_tray;
CaptureSession g_session;

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
    TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screenPoint.x,
                     screenPoint.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void DumpGeometryAndOpenLog() {
    LogDesktopGeometry(L"트레이 메뉴 요청");
    const wchar_t* path = log::FilePath();
    if (path != nullptr && path[0] != L'\0') {
        ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// 수식키 감시.
//
// 키보드 훅을 쓰지 않으므로(백신 오탐 프로파일) 여기서 GetAsyncKeyState로 본다.
// 마우스가 최근에 움직였을 때만 돌고, 조용해지면 스스로 멈춘다.
// 상주하는 동안 계속 깨어 있지 않기 위해서다.
constexpr UINT_PTR kModifierTimerId = 2;
constexpr UINT kModifierTimerMs = 40;
constexpr ULONGLONG kIdleStopMs = 2000;    // 이만큼 마우스가 조용하면 타이머를 끈다
constexpr ULONGLONG kPrewarmRefreshMs = 1500;  // 계속 누르고 있으면 프레임이 묵는다

bool g_modifierTimerRunning = false;
bool g_modifiersHeld = false;
ULONGLONG g_lastMouseActivity = 0;
ULONGLONG g_lastPrewarm = 0;

void PollModifiers(HWND hwnd) {
    const ULONGLONG now = GetTickCount64();
    const bool held = config::ModifiersHeld();

    if (held != g_modifiersHeld) {
        g_modifiersHeld = held;
        g_lastPrewarm = now;
        if (held) {
            g_session.Prewarm();
        } else {
            g_session.DropPrewarm();
        }
    } else if (held && now - g_lastPrewarm >= kPrewarmRefreshMs) {
        g_lastPrewarm = now;
        g_session.Prewarm();
    }

    if (!held && now - g_lastMouseActivity > kIdleStopMs) {
        KillTimer(hwnd, kModifierTimerId);
        g_modifierTimerRunning = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        g_tray.Restore();
        return 0;
    }

    switch (msg) {
        // --- 훅이 던지는 캡처 메시지 ---
        // 훅 콜백은 플래그만 세우고 즉시 반환한다. 실제 작업은 전부 여기서 한다.
        case hook::WM_SC_DRAG_BEGIN:
            g_session.Begin(hwnd);
            return 0;

        case hook::WM_SC_DRAG_UPDATE:
            g_session.Update();
            return 0;

        case hook::WM_SC_DRAG_END:
            g_session.Finish(hwnd);
            return 0;

        case hook::WM_SC_DRAG_CANCEL:
            g_session.Cancel(L"우클릭");
            return 0;

        // 마우스가 움직였다. 수식키 감시 타이머를 살려 둔다.
        case hook::WM_SC_MOUSE_ACTIVE:
            g_lastMouseActivity = GetTickCount64();
            if (!g_modifierTimerRunning) {
                SetTimer(hwnd, kModifierTimerId, kModifierTimerMs, nullptr);
                g_modifierTimerRunning = true;
            }
            return 0;

        case WM_TIMER:
            // 드래그 중 ESC 확인. 키보드 훅을 쓰지 않으므로 여기서 본다.
            if (wparam == kEscapeTimerId) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    hook::CancelDrag();
                    g_session.Cancel(L"ESC");
                }
                return 0;
            }
            if (wparam == kModifierTimerId) {
                PollModifiers(hwnd);
                return 0;
            }
            break;

        case kTrayCallbackMessage: {
            // NOTIFYICON_VERSION_4: wParam은 화면 좌표, lParam 하위 워드가 이벤트.
            const UINT event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU) {
                const POINT pt{static_cast<LONG>(GET_X_LPARAM(wparam)),
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
        case WM_DISPLAYCHANGE:
            g_session.Cancel(L"모니터 구성 변경");
            LogDesktopGeometry(L"WM_DISPLAYCHANGE");
            return 0;

        case WM_DPICHANGED:
            SC_LOG(L"WM_DPICHANGED: dpi=%u", LOWORD(wparam));
            LogDesktopGeometry(L"WM_DPICHANGED");
            return 0;

        case WM_DESTROY:
            g_session.Cancel(L"종료");
            hook::Remove();
            g_session.Shutdown();
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
                                WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance,
                                nullptr);
    if (hwnd == nullptr) {
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

    // WIC(PNG 인코딩)와 셸 API가 COM을 쓴다.
    const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);

    // 매니페스트가 이미 Per-Monitor V2를 선언하지만, 실제로 무엇이 적용됐는지
    // 확인해서 로그로 남긴다. 매니페스트가 안 먹은 채 좌표가 틀어지는 상황을
    // 추측으로 쫓지 않기 위해서다.
    {
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(ctx);
        const bool isPerMonitorV2 =
            AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) !=
            FALSE;
        SC_LOG(L"DPI 인식: awareness=%d, PerMonitorV2=%s", static_cast<int>(awareness),
               isPerMonitorV2 ? L"예" : L"아니오 (매니페스트 확인 필요)");
    }

    LogDesktopGeometry(L"시작");

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreated == 0) {
        SC_LOG(L"RegisterWindowMessage(TaskbarCreated) 실패 err=%lu", GetLastError());
    }

    HWND hwnd = CreateHostWindow(instance);
    if (hwnd == nullptr) {
        log::Shutdown();
        return 1;
    }

    if (!g_tray.Add(hwnd, kTrayCallbackMessage, kTrayIconId)) {
        // 트레이가 유일한 진입점이라 여기서 실패하면 사용자가 앱을 볼 방법이 없다.
        MessageBoxW(nullptr, L"트레이 아이콘을 등록하지 못했습니다.", SWEEPCAP_NAME_W,
                    MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        log::Shutdown();
        return 1;
    }

    g_session.Init(hwnd);

    if (!hook::Install(hwnd)) {
        // 훅이 없으면 영역 캡처가 안 된다. 트레이는 남겨 두어 종료할 수 있게 한다.
        MessageBoxW(nullptr,
                    L"마우스 훅을 설치하지 못했습니다.\n영역 캡처가 동작하지 않습니다.",
                    SWEEPCAP_NAME_W, MB_ICONWARNING | MB_OK);
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
