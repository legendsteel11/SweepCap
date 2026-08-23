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

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                           LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }

    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        // Remembered unconditionally. Whether this drag matters is not known
        // until it ends, because the modifier is pressed part way through.
        const bool got = VisibleFrame(hwnd, &g_startFrame);
        g_dragging = got ? hwnd : nullptr;
        SC_LOG(L"[창스냅] 드래그 시작 hwnd=%p 프레임=%s", hwnd, got ? L"읽음" : L"실패");
        return;
    }
    if (event != EVENT_SYSTEM_MOVESIZEEND) {
        return;
    }

    const HWND dragged = g_dragging;
    g_dragging = nullptr;

    // Every gate is logged. Nothing happening at all gives no clue which one
    // turned it away, and the gates depend on key state at one instant, which
    // cannot be reconstructed afterwards.
    const bool sameWindow = (dragged == hwnd);
    const bool held = settings::ModifiersHeld();
    const bool snappable = Snappable(hwnd);
    SC_LOG(L"[창스냅] 드래그 끝 hwnd=%p 같은창=%d 수식키=%d 대상가능=%d", hwnd,
           sameWindow ? 1 : 0, held ? 1 : 0, snappable ? 1 : 0);
    if (!sameWindow || !held || !snappable) {
        return;
    }

    RECT frame{};
    RECT window{};
    if (!VisibleFrame(hwnd, &frame) || !GetWindowRect(hwnd, &window)) {
        SC_LOG(L"[창스냅] 사각형을 읽지 못했다.");
        return;
    }

    // Only the edges the user dragged are moved. Snapping the others would
    // shift the window away from where it was left, which reads as the
    // application jumping rather than as a size being tidied up.
    const bool movedLeft = frame.left != g_startFrame.left;
    const bool movedTop = frame.top != g_startFrame.top;
    const bool movedRight = frame.right != g_startFrame.right;
    const bool movedBottom = frame.bottom != g_startFrame.bottom;

    // All four edges moving by the same amount is a move, not a resize, and
    // position snapping is off: a window dragged somewhere stays there.
    const bool resized = (frame.right - frame.left) != (g_startFrame.right - g_startFrame.left) ||
                         (frame.bottom - frame.top) != (g_startFrame.bottom - g_startFrame.top);
    SC_LOG(L"[창스냅] 시작(%ld,%ld,%ld,%ld) 끝(%ld,%ld,%ld,%ld) 움직인변=%c%c%c%c 크기변경=%d",
           g_startFrame.left, g_startFrame.top, g_startFrame.right, g_startFrame.bottom,
           frame.left, frame.top, frame.right, frame.bottom, movedLeft ? L'L' : L'-',
           movedTop ? L'T' : L'-', movedRight ? L'R' : L'-', movedBottom ? L'B' : L'-',
           resized ? 1 : 0);
    if (!resized) {
        return;
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
        SC_LOG(L"[창스냅] 격자에 붙이면 크기가 0이 된다. 건드리지 않는다.");
        return;
    }
    if (EqualRect(&target, &frame)) {
        return;  // already on the grid
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
    SC_LOG(L"[창스냅] %s (%ld,%ld,%ld,%ld) -> (%ld,%ld,%ld,%ld) 여백 %ld/%ld/%ld/%ld",
           ok ? L"적용" : L"SetWindowPos 실패", frame.left, frame.top, frame.right,
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
        SC_LOG(L"[창스냅] SetWinEventHook 실패 err=%lu", GetLastError());
        return false;
    }
    SC_LOG(L"[창스냅] 창 이벤트 훅 설치됨");
    return true;
}

void Remove() {
    if (g_hook != nullptr) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
    g_dragging = nullptr;
}

}  // namespace sc::winsnap
