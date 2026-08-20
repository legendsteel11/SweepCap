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

// Loads a user-visible string from the string table.
//
// The fallback only fires when the resource is missing, which means a broken
// build; it is English on purpose so it never has to be translated.
const wchar_t* LoadText(UINT id, const wchar_t* fallback, wchar_t* buffer, int count) {
    if (LoadStringW(GetModuleHandleW(nullptr), id, buffer, count) == 0) {
        wcscpy_s(buffer, static_cast<size_t>(count), fallback);
    }
    return buffer;
}

void ShowTrayMenu(HWND hwnd, POINT screenPoint) {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return;
    }

    wchar_t text[256];

#if defined(_DEBUG)
    AppendMenuW(menu.get(), MF_STRING, IDM_DUMP_GEOMETRY,
                LoadText(IDS_MENU_DUMP, L"Write coordinate log", text, ARRAYSIZE(text)));
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
#endif

    AppendMenuW(menu.get(), MF_STRING, IDM_EXIT,
                LoadText(IDS_MENU_EXIT, L"Exit", text, ARRAYSIZE(text)));

    // Taking the foreground first is what lets a click outside the menu dismiss
    // it; posting WM_NULL afterwards is the other half of that same fix.
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

// Modifier watch.
//
// No keyboard hook is installed, so the modifiers are polled here with
// GetAsyncKeyState. The timer runs only while the mouse has moved recently and
// stops itself once things go quiet, so nothing wakes up while the application
// sits idle in the tray.
constexpr UINT_PTR kModifierTimerId = 2;
constexpr UINT kModifierTimerMs = 40;
constexpr ULONGLONG kIdleStopMs = 2000;        // stop the timer after this much silence
constexpr ULONGLONG kPrewarmRefreshMs = 1500;  // a held modifier lets the frame go stale

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
        // Capture messages from the hook. The callback only sets state and
        // posts; all the real work happens here.
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

        // The mouse moved: keep the modifier-watch timer alive.
        case hook::WM_SC_MOUSE_ACTIVE:
            g_lastMouseActivity = GetTickCount64();
            if (!g_modifierTimerRunning) {
                SetTimer(hwnd, kModifierTimerId, kModifierTimerMs, nullptr);
                g_modifierTimerRunning = true;
            }
            return 0;

        case WM_TIMER:
            if (wparam == kEscapeTimerId) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    hook::CancelDrag();
                    g_session.Cancel(L"ESC");
                    return 0;
                }
                // Shift changes and the whole-window highlight both have to
                // land even while the mouse is standing still.
                g_session.Tick();
                return 0;
            }
            if (wparam == kModifierTimerId) {
                PollModifiers(hwnd);
                return 0;
            }
            break;

        case kTrayCallbackMessage: {
            // NOTIFYICON_VERSION_4: wParam carries screen coordinates and the
            // low word of lParam carries the event.
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

        // Instrumentation: re-dump the coordinates whenever the monitor
        // configuration changes.
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

// The tray-resident window.
//
// A message-only window (HWND_MESSAGE) looks like the obvious choice and is the
// wrong one: broadcasts such as TaskbarCreated only reach top-level windows.
// So this is a real top-level window that is never shown, with WS_EX_TOOLWINDOW
// keeping it out of the taskbar and Alt+Tab.
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
    // Single instance. A second launch withdraws quietly.
    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, SWEEPCAP_MUTEX_W)};
    const bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    log::Init();
    SC_LOG(L"%s %s 시작", SWEEPCAP_NAME_W, SWEEPCAP_VERSION_W);

    if (alreadyRunning) {
        SC_LOG(L"이미 실행 중이라 종료한다.");
        return 0;
    }

    // WIC (PNG encoding) and the shell APIs need COM.
    const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);

    // The manifest already declares Per-Monitor V2, but record what actually
    // took effect. Without this, a manifest that failed to apply would show up
    // much later as coordinates that are subtly wrong.
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

    wchar_t message[512];

    if (!g_tray.Add(hwnd, kTrayCallbackMessage, kTrayIconId)) {
        // The tray icon is the only entry point, so there is no way for the
        // user to reach the application if this fails.
        MessageBoxW(nullptr,
                    LoadText(IDS_ERR_TRAY, L"Could not register the tray icon.", message,
                             ARRAYSIZE(message)),
                    SWEEPCAP_NAME_W, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        log::Shutdown();
        return 1;
    }

    g_session.Init(hwnd);

    if (!hook::Install(hwnd)) {
        // Region capture is dead without the hook, but the tray icon stays so
        // the user can still exit.
        MessageBoxW(nullptr,
                    LoadText(IDS_ERR_HOOK,
                             L"Could not install the mouse hook.\nRegion capture is disabled.",
                             message, ARRAYSIZE(message)),
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
