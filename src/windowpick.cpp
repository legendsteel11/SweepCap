#include "windowpick.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <cmath>
#include <vector>

#include "capture.h"
#include "log.h"

namespace sc {
namespace {

// Anything smaller than one grid cell is an artefact rather than a window
// someone means to capture. The floor is deliberately low: tray utilities are
// often narrow strips, and rejecting them is worse than occasionally offering
// a small helper window.
constexpr LONG kMinWindowSide = 32;

// The desktop itself. Clicking empty space should fall through to the corner
// rule and then to an ordinary drag, not capture the entire screen.
bool IsDesktopClass(HWND hwnd) {
    wchar_t cls[32] = L"";
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

// Windows 11 rounds corners at 8 DIPs, or 4 for the small variant.
constexpr int kCornerRadiusDip = 8;
constexpr int kCornerRadiusSmallDip = 4;

struct Candidate {
    HWND hwnd;
    RECT frame;
};

struct EnumState {
    DWORD ownProcess;
    std::vector<Candidate> found;
};

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != FALSE;
}

bool IsPickable(HWND hwnd, DWORD ownProcess, RECT* outFrame);

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<EnumState*>(param);
    RECT frame{};
    if (IsPickable(hwnd, state->ownProcess, &frame)) {
        state->found.push_back(Candidate{hwnd, frame});
    }
    return TRUE;
}

// True when the given corner of the window is actually the visible pixel there.
// Probes just inside the window so the test does not land on a neighbour.
bool CornerIsExposed(HWND hwnd, const RECT& frame, int cornerIndex) {
    constexpr LONG kInset = 3;
    POINT probe{};
    switch (cornerIndex) {
        case 0: probe = {frame.left + kInset, frame.top + kInset}; break;
        case 1: probe = {frame.right - kInset, frame.top + kInset}; break;
        case 2: probe = {frame.left + kInset, frame.bottom - kInset}; break;
        default: probe = {frame.right - kInset, frame.bottom - kInset}; break;
    }
    HWND at = WindowFromPoint(probe);
    if (at == nullptr) {
        return false;
    }
    at = GetAncestor(at, GA_ROOT);
    return at == hwnd;
}

int CornerRadiusFor(HWND hwnd, const RECT& frame) {
    // A maximized window has square corners.
    if (IsZoomed(hwnd)) {
        return 0;
    }

    int dip = kCornerRadiusDip;
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_DEFAULT;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                        sizeof(preference)))) {
        if (preference == DWMWCP_DONOTROUND) {
            return 0;
        }
        if (preference == DWMWCP_ROUNDSMALL) {
            dip = kCornerRadiusSmallDip;
        }
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    const POINT centre{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
    HMONITOR monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr) {
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    }
    return MulDiv(dip, static_cast<int>(dpiX), 96);
}

// The shared filter: is this a top-level window worth offering as a capture
// target? Both picking rules go through it, so they cannot drift apart.
//
// WS_EX_TOOLWINDOW is not a rejection. That style is exactly what keeps a
// window out of Alt+Tab, which is what tray utilities set, and filtering on it
// made every tray-only application uncapturable.
bool IsPickable(HWND hwnd, DWORD ownProcess, RECT* outFrame) {
    if (hwnd == nullptr || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == ownProcess) {
        return false;
    }
    if (IsDesktopClass(hwnd)) {
        return false;
    }
    if (IsCloaked(hwnd)) {
        return false;
    }
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        return false;
    }
    if (frame.right - frame.left < kMinWindowSide ||
        frame.bottom - frame.top < kMinWindowSide) {
        return false;
    }
    if (outFrame != nullptr) {
        *outFrame = frame;
    }
    return true;
}

}  // namespace

bool PickWindowAt(POINT pt, WindowPick* out) {
    if (out == nullptr) {
        return false;
    }
    HWND hwnd = WindowFromPoint(pt);
    if (hwnd == nullptr) {
        return false;
    }
    // WindowFromPoint lands on the deepest child control; walk up to the frame.
    hwnd = GetAncestor(hwnd, GA_ROOT);

    RECT frame{};
    if (!IsPickable(hwnd, GetCurrentProcessId(), &frame)) {
        return false;
    }
    out->hwnd = hwnd;
    out->frame = frame;
    out->cornerRadius = CornerRadiusFor(hwnd, frame);
    return true;
}

bool PickWindowByCorner(const RECT& cell, WindowPick* out) {
    if (out == nullptr) {
        return false;
    }

    EnumState state{};
    state.ownProcess = GetCurrentProcessId();
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&state));

    // EnumWindows walks front to back, so the first match is the topmost.
    for (const Candidate& c : state.found) {
        const POINT corners[4] = {{c.frame.left, c.frame.top},
                                  {c.frame.right, c.frame.top},
                                  {c.frame.left, c.frame.bottom},
                                  {c.frame.right, c.frame.bottom}};
        for (int i = 0; i < 4; ++i) {
            if (!PtInRect(&cell, corners[i])) {
                continue;
            }
            if (!CornerIsExposed(c.hwnd, c.frame, i)) {
                continue;  // the corner is buried under another window
            }
            out->hwnd = c.hwnd;
            out->frame = c.frame;
            out->cornerRadius = CornerRadiusFor(c.hwnd, c.frame);
            return true;
        }
    }
    return false;
}

bool PickMonitorAt(POINT pt, WindowPick* out) {
    if (out == nullptr) {
        return false;
    }
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    out->hwnd = nullptr;
    out->frame = info.rcMonitor;  // the whole screen, taskbar included
    out->cornerRadius = 0;
    return true;
}

void CarveRoundedCorners(Bitmap32& bitmap, int radius) {
    if (radius <= 0 || !bitmap.Valid()) {
        return;
    }
    if (bitmap.width < radius * 2 || bitmap.height < radius * 2) {
        return;
    }

    const double r = static_cast<double>(radius);
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            // Distance from the centre of the corner arc.
            const double dx = r - 0.5 - static_cast<double>(x);
            const double dy = r - 0.5 - static_cast<double>(y);
            const double distance = std::sqrt(dx * dx + dy * dy);
            if (distance <= r - 0.5) {
                continue;  // fully inside
            }

            // A half-pixel band around the arc keeps the edge from stair-stepping.
            double coverage = 0.0;
            if (distance < r + 0.5) {
                coverage = (r + 0.5) - distance;
            }
            const auto alpha = static_cast<uint32_t>(coverage * 255.0 + 0.5);

            const int right = bitmap.width - 1 - x;
            const int bottom = bitmap.height - 1 - y;
            const int xs[2] = {x, right};
            const int ys[2] = {y, bottom};
            for (const int py : ys) {
                uint32_t* row = bitmap.Row(py);
                for (const int px : xs) {
                    if (alpha == 0) {
                        row[px] = 0;  // clear colour too, so no fringe shows
                    } else {
                        row[px] = (row[px] & 0x00FFFFFFu) | (alpha << 24);
                    }
                }
            }
        }
    }
}

}  // namespace sc
