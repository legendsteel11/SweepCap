#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Coordinate convention.
//
//   - All internal coordinates are physical pixels.
//   - The origin is the top-left of the virtual desktop, so X and Y can be
//     negative when a secondary monitor sits left of or above the primary one.
//   - Window rectangles come from DWMWA_EXTENDED_FRAME_BOUNDS, not
//     GetWindowRect. Windows 11 windows carry invisible resize padding outside
//     the visible border.
//   - Grid and placement work against MONITORINFO::rcWork, not rcMonitor.
//
// This convention reaches every part of the code. Changing it later means
// re-verifying all of it.

namespace sc {

struct MonitorGeometry {
    std::wstring device;
    RECT monitor{};   // full monitor area
    RECT work{};      // work area; grid and placement use this one
    UINT dpi = 96;    // effective DPI; 96 means 100%
    bool primary = false;

    LONG MonitorWidth() const { return monitor.right - monitor.left; }
    LONG MonitorHeight() const { return monitor.bottom - monitor.top; }
    LONG WorkWidth() const { return work.right - work.left; }
    LONG WorkHeight() const { return work.bottom - work.top; }
    UINT ScalePercent() const { return dpi * 100 / 96; }
};

struct DesktopGeometry {
    RECT bounds{};    // virtual desktop; left and top can be negative
    std::vector<MonitorGeometry> monitors;

    LONG Width() const { return bounds.right - bounds.left; }
    LONG Height() const { return bounds.bottom - bounds.top; }
    bool HasMixedDpi() const;
    bool HasNegativeOrigin() const { return bounds.left < 0 || bounds.top < 0; }
};

// Reads the current monitor layout. The caller process must be Per-Monitor V2
// aware for these to come back as physical pixels.
DesktopGeometry QueryDesktop();

// The visible rectangle of a window. Windows 11 windows have invisible resize
// padding outside the visible border, so GetWindowRect drags in background.
// usedDwm reports whether the DWM value was used (true) or whether the call
// fell back to GetWindowRect (false).
RECT WindowFrameBounds(HWND hwnd, bool* usedDwm = nullptr);

// Instrumentation: dump the current layout to the debug log.
// reason identifies what triggered the dump.
void LogDesktopGeometry(const wchar_t* reason);

}  // namespace sc
