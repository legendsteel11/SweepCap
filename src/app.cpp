#include "app.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <string>

#include "../res/resource.h"
#include "app_name.h"
#include "config.h"
#include "coords.h"
#include "hook.h"
#include "log.h"
#include "session.h"
#include "settings.h"
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

// Everything the user can change is a menu item.
//
// The settings are all single choices from a short list, which a radio
// submenu shows and changes in one click while the current value is visible
// without opening anything. A settings window would need two clicks more per
// change and a second place for the same values to drift out of sync.

// Keeps the tail of a long path, which is the part that says which folder it
// is, and marks the cut so a shortened path is never mistaken for a real one.
std::wstring ShortenPath(const std::wstring& path) {
    constexpr size_t kMaxChars = 44;
    if (path.size() <= kMaxChars) {
        return path;
    }
    return L"..." + path.substr(path.size() - (kMaxChars - 3));
}

// Each builder keeps its own string buffer rather than borrowing the caller's.
// Sharing one buffer with the caller is unsafe here: a builder and the
// LoadText that reads the submenu's own title are arguments of the same call,
// and the order those are evaluated in is not defined. Sharing the buffer put
// the last item's text in the submenu title.

wil::unique_hmenu BuildGestureMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    wchar_t label[256];
    size_t total = 0;
    const settings::Gesture* items = settings::Gestures(&total);
    for (size_t i = 0; i < total; ++i) {
        AppendMenuW(menu.get(), MF_STRING, IDM_GESTURE_FIRST + i,
                    LoadText(items[i].labelId, L"Modifier", label, ARRAYSIZE(label)));
    }
    const UINT current = IDM_GESTURE_FIRST + static_cast<UINT>(settings::GestureIndex());
    CheckMenuRadioItem(menu.get(), IDM_GESTURE_FIRST,
                       IDM_GESTURE_FIRST + static_cast<UINT>(total) - 1, current,
                       MF_BYCOMMAND);
    return menu;
}

wil::unique_hmenu BuildGridMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    wchar_t format[64];
    LoadText(IDS_GRID_DIVISIONS, L"%d x %d divisions", format, ARRAYSIZE(format));

    size_t total = 0;
    const settings::GridChoice* choices = settings::GridChoices(&total);
    bool separated = false;
    for (size_t i = 0; i < total; ++i) {
        // Both kinds share one radio list, so the value carries the mode and
        // there is nothing to choose before choosing a value. A separator marks
        // where one kind ends and the other begins.
        if (choices[i].ByDivision() && !separated) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            separated = true;
        }
        wchar_t label[64];
        const int written = choices[i].ByDivision()
                                ? swprintf_s(label, format, choices[i].cols, choices[i].rows)
                                : swprintf_s(label, L"%d px", choices[i].px);
        if (written < 0) {
            continue;
        }
        AppendMenuW(menu.get(), MF_STRING, IDM_GRID_FIRST + i, label);
    }
    const UINT current = IDM_GRID_FIRST + static_cast<UINT>(settings::GridIndex());
    CheckMenuRadioItem(menu.get(), IDM_GRID_FIRST,
                       IDM_GRID_FIRST + static_cast<UINT>(total) - 1, current, MF_BYCOMMAND);
    return menu;
}

wil::unique_hmenu BuildFolderMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    wchar_t label[256];
    // The current folder heads the submenu as a greyed line. It is the one
    // setting whose value is a path, so a checkmark cannot express it.
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0,
                ShortenPath(settings::CaptureRoot()).c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, IDM_FOLDER_CHANGE,
                LoadText(IDS_MENU_FOLDER_CHANGE, L"Change...", label, ARRAYSIZE(label)));
    AppendMenuW(
        menu.get(), MF_STRING | (settings::CaptureRootIsDefault() ? MF_GRAYED : 0),
        IDM_FOLDER_DEFAULT,
        LoadText(IDS_MENU_FOLDER_DEFAULT, L"Restore default folder", label, ARRAYSIZE(label)));
    return menu;
}

// MF_POPUP hands the submenu to the parent, which destroys it in turn, so the
// wrapper has to let go of it.
void AttachSubmenu(HMENU parent, wil::unique_hmenu submenu, const wchar_t* label) {
    if (!submenu) {
        return;
    }
    if (AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu.get()), label)) {
        submenu.release();
    }
}

void ShowTrayMenu(HWND hwnd, POINT screenPoint) {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return;
    }

    wchar_t text[256];

    AppendMenuW(menu.get(), MF_STRING, IDM_OPEN_FOLDER,
                LoadText(IDS_MENU_OPEN_FOLDER, L"Open capture folder", text, ARRAYSIZE(text)));
    SetMenuDefaultItem(menu.get(), IDM_OPEN_FOLDER, FALSE);
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);

    AttachSubmenu(menu.get(), BuildGestureMenu(),
                  LoadText(IDS_MENU_GESTURE, L"Change shortcut", text, ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildGridMenu(),
                  LoadText(IDS_MENU_GRID, L"Capture grid size", text, ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildFolderMenu(),
                  LoadText(IDS_MENU_FOLDER, L"Save folder", text, ARRAYSIZE(text)));

    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (settings::RunAtStartup() ? MF_CHECKED : MF_UNCHECKED),
                IDM_RUN_AT_STARTUP,
                LoadText(IDS_MENU_STARTUP, L"Run at startup", text, ARRAYSIZE(text)));

#if defined(_DEBUG)
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, IDM_DUMP_GEOMETRY,
                LoadText(IDS_MENU_DUMP, L"Write coordinate log", text, ARRAYSIZE(text)));
#endif

    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, IDM_EXIT,
                LoadText(IDS_MENU_EXIT, L"Exit", text, ARRAYSIZE(text)));

    // Taking the foreground first is what lets a click outside the menu dismiss
    // it; posting WM_NULL afterwards is the other half of that same fix.
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screenPoint.x,
                     screenPoint.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

// Opens today's folder when there is one, and the root otherwise, since that
// is where the next capture will land.
void OpenCaptureFolder() {
    const std::wstring& root = settings::CaptureRoot();
    if (root.empty()) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t today[MAX_PATH];
    if (swprintf_s(today, L"%s\\%04u-%02u-%02u", root.c_str(), now.wYear, now.wMonth,
                   now.wDay) >= 0 &&
        GetFileAttributesW(today) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", today, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    // Before the first capture of the day the root may not exist yet.
    SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ChangeCaptureFolder(HWND owner) {
    auto dialog = wil::CoCreateInstanceNoThrow<IFileOpenDialog>(CLSID_FileOpenDialog);
    if (!dialog) {
        SC_LOG(L"[설정] 폴더 선택 대화상자를 만들지 못했다.");
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        // FORCEFILESYSTEM keeps the result to somewhere that has a real path;
        // without it a virtual shell location can be chosen and then not
        // written to.
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }

    wchar_t text[256];
    dialog->SetTitle(
        LoadText(IDS_FOLDER_PICK_TITLE, L"Choose where captures are saved", text,
                 ARRAYSIZE(text)));

    // Start where captures currently go, so this reads as changing the folder
    // rather than choosing one from nothing.
    wil::com_ptr_nothrow<IShellItem> start;
    if (SUCCEEDED(SHCreateItemFromParsingName(settings::CaptureRoot().c_str(), nullptr,
                                              IID_PPV_ARGS(&start)))) {
        dialog->SetFolder(start.get());
    }

    // The owner window is hidden, so the dialog needs help reaching the front.
    SetForegroundWindow(owner);
    if (FAILED(dialog->Show(owner))) {
        return;  // cancelled
    }

    wil::com_ptr_nothrow<IShellItem> item;
    wil::unique_cotaskmem_string path;
    if (FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return;
    }
    settings::SetCaptureRoot(path.get());
}

// The same failure is not reported again inside this window.
//
// Whatever breaks a save (a folder that is gone, a full disk, a denied
// permission) breaks the next one too, and a capture takes well under a second
// here. Without this, a broken folder turns a burst of captures into a queue of
// balloons that keeps appearing long after the user has stopped. Telling them
// once is the whole value; repeating it is only noise.
//
// A different failure reports immediately, so a new problem is never hidden
// behind an old one.
constexpr ULONGLONG kFailureRepeatMs = 30000;
ULONGLONG g_lastFailureAt = 0;
WPARAM g_lastFailureKind = 0;

// Says which part of the delivery failed, so the user knows whether the
// capture is lost or still sitting somewhere usable.
void ReportDeliveryFailure(WPARAM failures) {
    const ULONGLONG now = GetTickCount64();
    if (failures == g_lastFailureKind && now - g_lastFailureAt < kFailureRepeatMs) {
        SC_LOG(L"[알림] 같은 실패가 %llu ms 안에 반복됐다. 알림을 생략한다.",
               now - g_lastFailureAt);
        return;
    }
    g_lastFailureKind = failures;
    g_lastFailureAt = now;

    UINT id = IDS_ERR_DELIVERY_ALL;
    if ((failures & kDeliveryCaptureFailed) != 0) {
        id = IDS_ERR_CAPTURE_NONE;
    } else if ((failures & kDeliverySaveFailed) != 0 &&
               (failures & kDeliveryClipboardFailed) == 0) {
        id = IDS_ERR_SAVE_ONLY;
    } else if ((failures & kDeliveryClipboardFailed) != 0 &&
               (failures & kDeliverySaveFailed) == 0) {
        id = IDS_ERR_CLIP_ONLY;
    }

    // Separate buffers: one call cannot fill the same one twice.
    wchar_t title[64];
    wchar_t text[256];
    g_tray.ShowBalloon(
        LoadText(IDS_ERR_CAPTURE_TITLE, L"Capture failed", title, ARRAYSIZE(title)),
        LoadText(id, L"The capture could not be delivered.", text, ARRAYSIZE(text)));
}

void ToggleRunAtStartup(HWND owner) {
    const bool next = !settings::RunAtStartup();
    if (settings::SetRunAtStartup(next)) {
        SC_LOG(L"[설정] 부팅 시 시작 %s", next ? L"활성화" : L"비활성화");
        return;
    }
    wchar_t text[256];
    MessageBoxW(owner,
                LoadText(IDS_ERR_STARTUP, L"Could not change the startup entry.", text,
                         ARRAYSIZE(text)),
                SWEEPCAP_NAME_W, MB_OK | MB_ICONWARNING);
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
    const bool held = settings::ModifiersHeld();

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
            if (wparam == kFlashTimerId) {
                g_session.EndFlash();
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
            } else if (event == WM_LBUTTONDBLCLK) {
                // Matches the item the menu marks as its default. A single
                // click does nothing, so brushing the icon has no effect.
                OpenCaptureFolder();
            }
            return 0;
        }

        case WM_SC_DELIVERY_FAILED:
            ReportDeliveryFailure(wparam);
            return 0;

        case WM_COMMAND: {
            // The radio submenus carry their index in the command id, so the
            // handler is a range test and a subtraction.
            const UINT id = LOWORD(wparam);
            if (id >= IDM_GESTURE_FIRST && id <= IDM_GESTURE_LAST) {
                settings::SetGestureIndex(static_cast<int>(id - IDM_GESTURE_FIRST));
                return 0;
            }
            if (id >= IDM_GRID_FIRST && id <= IDM_GRID_LAST) {
                settings::SetGridIndex(static_cast<int>(id - IDM_GRID_FIRST));
                return 0;
            }
            switch (id) {
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_OPEN_FOLDER:
                    OpenCaptureFolder();
                    return 0;
                case IDM_FOLDER_CHANGE:
                    ChangeCaptureFolder(hwnd);
                    return 0;
                case IDM_FOLDER_DEFAULT:
                    settings::SetCaptureRoot(nullptr);
                    return 0;
                case IDM_RUN_AT_STARTUP:
                    ToggleRunAtStartup(hwnd);
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
        }

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

    // Before the hook goes in: the gesture it tests for comes from here.
    settings::Load();

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
    // Baseline to compare every later reading against.
    SC_LOG_RESOURCES(L"시작");

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
