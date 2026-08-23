#include "winsnap.h"

#include <dwmapi.h>

#include "grid.h"
#include "log.h"
#include "settings.h"

namespace sc::winsnap {
namespace {

HWINEVENTHOOK g_hook = nullptr;

// The window being dragged and the frame it started with, so the end of the
// drag can tell which edges the user actually moved.
HWND g_dragging = nullptr;
RECT g_startFrame{};

// Preview of where the release will put the window.
//
// The drag itself cannot be made to stick to the grid. A system loop owns the
// window rectangle while a resize is running and recomputes it from the cursor
// on every mouse move; the only supported way to constrain that is WM_SIZING,
// which is delivered to the target window's own procedure and so needs a DLL
// inside its process. Calling SetWindowPos into the running loop is overridden
// on the next mouse move and the window only judders.
//
// So the outline shows where the release will land instead, and the jump on
// release stops being a surprise. This is the same bargain FancyZones makes,
// for the same reason.
constexpr wchar_t kGhostClass[] = L"SweepCap.SnapGhost";
constexpr COLORREF kGhostColour = RGB(96, 156, 255);  // the whole-window accent
constexpr int kGhostThickness = 4;
constexpr UINT_PTR kGhostTimerId = 1;
constexpr UINT kGhostTickMs = 30;

HWND g_ghost = nullptr;
RECT g_ghostRect{};
bool g_ghostVisible = false;

// What the window looks like, as opposed to where its window rectangle is.
//
// Windows 11 leaves an invisible resize margin outside the visible border, and
// it is not the same on every side: measured at left 7, top 0, right 7,
// bottom 7. Snapping the window rectangle would leave the visible edges off the
// grid by that much, which is exactly what stops two snapped windows from
// sitting flush against each other.
bool VisibleFrame(HWND hwnd, RECT* out) {
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out,
                                           sizeof(*out)));
}

// The work area of the monitor the window is on. Not the whole monitor: a
// window must not end up under the taskbar.
RECT WorkAreaFor(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return RECT{0, 0, 1920, 1080};
}

// Windows that are not the user's to resize, or that cannot be resized at all.
bool Snappable(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    // A maximized or snapped window has no size of its own to adjust; the shell
    // owns it, and pushing a new rectangle in leaves it in a state where the
    // restore button lies about what it will restore to.
    if (IsZoomed(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return (style & WS_THICKFRAME) != 0;
}

// Where the release would put the window, or false when nothing would happen.
bool SnapTarget(HWND hwnd, const RECT& frame, RECT* out);

LRESULT CALLBACK GhostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

bool EnsureGhost() {
    if (g_ghost != nullptr) {
        return true;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GhostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kGhostClass;
        // A solid background brush is the whole of the painting. The window
        // region clips it to a hollow frame, so nothing else is needed.
        wc.hbrBackground = CreateSolidBrush(kGhostColour);
        if (RegisterClassExW(&wc) == 0) {
            SC_LOG(L"[winsnap] ghost window class registration failed err=%lu", GetLastError());
            return false;
        }
        registered = true;
    }

    // WS_EX_TRANSPARENT is the important one: the outline must never take a
    // click, or it would interrupt the very drag it is describing.
    //
    // Deliberately not layered. A layered window keeps a composition surface
    // the size of the whole window even when a region leaves only a four pixel
    // frame showing, and rebuilding a 4K-sized surface thirty times a second
    // was most of the cost of an early version of this. The region alone gives
    // the same picture for nothing.
    g_ghost = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kGhostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (g_ghost == nullptr) {
        SC_LOG(L"[winsnap] ghost window creation failed err=%lu", GetLastError());
        return false;
    }
    return true;
}

void HideGhost() {
    if (g_ghost != nullptr && g_ghostVisible) {
        ShowWindow(g_ghost, SW_HIDE);
    }
    g_ghostVisible = false;
}

void ShowGhost(const RECT& r) {
    if (!EnsureGhost()) {
        return;
    }
    const int w = static_cast<int>(r.right - r.left);
    const int h = static_cast<int>(r.bottom - r.top);
    if (w <= kGhostThickness * 2 || h <= kGhostThickness * 2) {
        HideGhost();
        return;
    }

    // The target only changes when the drag crosses a grid line, so almost
    // every tick asks for the rectangle that is already on screen. Doing the
    // work anyway is what made this expensive: thirty pointless window
    // operations a second, each of them repainting.
    if (g_ghostVisible && EqualRect(&r, &g_ghostRect)) {
        return;
    }

    // The region depends on the size alone, so moving without resizing leaves
    // it as it is.
    const bool resized = (w != g_ghostRect.right - g_ghostRect.left) ||
                         (h != g_ghostRect.bottom - g_ghostRect.top);
    if (!g_ghostVisible || resized) {
        // Hollow it out, so what shows is a frame and the window underneath
        // stays visible through the middle.
        HRGN outer = CreateRectRgn(0, 0, w, h);
        HRGN inner = CreateRectRgn(kGhostThickness, kGhostThickness, w - kGhostThickness,
                                   h - kGhostThickness);
        CombineRgn(outer, outer, inner, RGN_DIFF);
        DeleteObject(inner);
        SetWindowRgn(g_ghost, outer, FALSE);  // the window owns the region now
    }

    SetWindowPos(g_ghost, HWND_TOPMOST, r.left, r.top, w, h,
                 SWP_NOACTIVATE | (g_ghostVisible ? 0 : SWP_SHOWWINDOW));
    g_ghostRect = r;
    g_ghostVisible = true;
}

// Runs while a drag is in progress. There is no event for "the window moved a
// bit", so the frame is read on a timer instead.
void CALLBACK GhostTick(HWND, UINT, UINT_PTR, DWORD) {
    const HWND target = g_dragging;
    if (target == nullptr) {
        HideGhost();
        return;
    }

    RECT frame{};
    RECT snapped{};
    if (!settings::ModifiersHeld() || !VisibleFrame(target, &frame) ||
        !SnapTarget(target, frame, &snapped)) {
        HideGhost();
        return;
    }
    ShowGhost(snapped);
}

LRESULT CALLBACK GhostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_TIMER && wparam == kGhostTimerId) {
        GhostTick(hwnd, msg, kGhostTimerId, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The one place the snapped rectangle is worked out.
//
// The preview and the release both come through here, so what the outline
// promises is exactly what happens. Two copies of this arithmetic would
// eventually disagree, and the disagreement would show up as the window
// landing somewhere other than where it was drawn.
bool SnapTarget(HWND hwnd, const RECT& frame, RECT* out) {
    if (!Snappable(hwnd)) {
        return false;
    }

    // Only the edges the user dragged are moved. Snapping the rest would shift
    // the window away from where it was left, which reads as the application
    // jumping rather than as a size being tidied up.
    const bool movedLeft = frame.left != g_startFrame.left;
    const bool movedTop = frame.top != g_startFrame.top;
    const bool movedRight = frame.right != g_startFrame.right;
    const bool movedBottom = frame.bottom != g_startFrame.bottom;

    // A move, not a resize, and position snapping is off: a window dragged
    // somewhere stays there.
    const bool resized =
        (frame.right - frame.left) != (g_startFrame.right - g_startFrame.left) ||
        (frame.bottom - frame.top) != (g_startFrame.bottom - g_startFrame.top);
    if (!resized) {
        return false;
    }

    const RECT work = WorkAreaFor(hwnd);
    const settings::GridChoice& grid = settings::WindowGrid();
    const GridAxis x = HorizontalAxis(work, grid);
    const GridAxis y = VerticalAxis(work, grid);

    RECT target = frame;
    if (movedLeft) {
        target.left = x.Snap(frame.left);
    }
    if (movedRight) {
        target.right = x.Snap(frame.right);
    }
    if (movedTop) {
        target.top = y.Snap(frame.top);
    }
    if (movedBottom) {
        target.bottom = y.Snap(frame.bottom);
    }

    // Rounding both edges of a narrow window onto the same line would collapse
    // it. Leaving it alone is better than resizing it to nothing.
    if (target.right <= target.left || target.bottom <= target.top) {
        return false;
    }
    *out = target;
    return true;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                           LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }

    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        // Remembered unconditionally. Whether this drag matters is not known
        // until it ends, because the modifier is pressed part way through.
        g_dragging = VisibleFrame(hwnd, &g_startFrame) ? hwnd : nullptr;
        if (g_dragging != nullptr && EnsureGhost()) {
            // There is no event for "the window moved a bit", so the preview
            // reads the frame on a timer for as long as the drag lasts.
            SetTimer(g_ghost, kGhostTimerId, kGhostTickMs, nullptr);
        }
        return;
    }
    if (event != EVENT_SYSTEM_MOVESIZEEND) {
        return;
    }

    const HWND dragged = g_dragging;
    g_dragging = nullptr;
    if (g_ghost != nullptr) {
        KillTimer(g_ghost, kGhostTimerId);
    }
    HideGhost();

    if (dragged != hwnd || !settings::ModifiersHeld()) {
        return;
    }

    RECT frame{};
    RECT window{};
    RECT target{};
    if (!VisibleFrame(hwnd, &frame) || !GetWindowRect(hwnd, &window) ||
        !SnapTarget(hwnd, frame, &target) || EqualRect(&target, &frame)) {
        return;
    }

    // Back from the visible frame to the window rectangle SetWindowPos takes.
    const RECT margin{frame.left - window.left, frame.top - window.top,
                      window.right - frame.right, window.bottom - frame.bottom};
    const int left = static_cast<int>(target.left - margin.left);
    const int top = static_cast<int>(target.top - margin.top);
    const int width = static_cast<int>(target.right + margin.right - left);
    const int height = static_cast<int>(target.bottom + margin.bottom - top);

    const BOOL ok = SetWindowPos(hwnd, nullptr, left, top, width, height,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SC_LOG(L"[winsnap] %s (%ld,%ld,%ld,%ld) -> (%ld,%ld,%ld,%ld) margins %ld/%ld/%ld/%ld",
           ok ? L"applied" : L"SetWindowPos failed", frame.left, frame.top, frame.right,
           frame.bottom, target.left, target.top, target.right, target.bottom, margin.left,
           margin.top, margin.right, margin.bottom);
}

}  // namespace

bool WindowDragActive() {
    return g_dragging != nullptr;
}

bool Install() {
    if (g_hook != nullptr) {
        return true;
    }
    // Out of context, so nothing of ours is injected into other processes and
    // the callback arrives on this thread through the message loop.
    g_hook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                             WinEventProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_hook == nullptr) {
        SC_LOG(L"[winsnap] SetWinEventHook failed err=%lu", GetLastError());
        return false;
    }
    SC_LOG(L"[winsnap] window event hook installed");
    return true;
}

void Remove() {
    if (g_hook != nullptr) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
    g_dragging = nullptr;
    if (g_ghost != nullptr) {
        KillTimer(g_ghost, kGhostTimerId);
        DestroyWindow(g_ghost);
        g_ghost = nullptr;
    }
    g_ghostVisible = false;
}

}  // namespace sc::winsnap
