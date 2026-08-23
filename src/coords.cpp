#include "coords.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>

#include "log.h"

namespace sc {
namespace {

BOOL CALLBACK EnumProc(HMONITOR handle, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<MonitorGeometry>*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) {
        return TRUE;  // one unreadable monitor should not stop the enumeration
    }

    MonitorGeometry geo;
    geo.device = info.szDevice;
    geo.monitor = info.rcMonitor;
    geo.work = info.rcWork;
    geo.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        geo.dpi = dpiX;
    }

    out->push_back(std::move(geo));
    return TRUE;
}

}  // namespace

bool DesktopGeometry::HasMixedDpi() const {
    if (monitors.size() < 2) {
        return false;
    }
    const UINT first = monitors.front().dpi;
    return std::any_of(monitors.begin(), monitors.end(),
                       [first](const MonitorGeometry& m) { return m.dpi != first; });
}

DesktopGeometry QueryDesktop() {
    DesktopGeometry desktop;

    // The virtual desktop comes from the SM_* metrics. Its origin can be negative.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    desktop.bounds.left = x;
    desktop.bounds.top = y;
    desktop.bounds.right = x + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bounds.bottom = y + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    EnumDisplayMonitors(nullptr, nullptr, EnumProc,
                        reinterpret_cast<LPARAM>(&desktop.monitors));

    // Primary first, then top-left order. This only exists to keep the log readable.
    std::sort(desktop.monitors.begin(), desktop.monitors.end(),
              [](const MonitorGeometry& a, const MonitorGeometry& b) {
                  if (a.primary != b.primary) {
                      return a.primary;
                  }
                  if (a.monitor.left != b.monitor.left) {
                      return a.monitor.left < b.monitor.left;
                  }
                  return a.monitor.top < b.monitor.top;
              });

    return desktop;
}

RECT WindowFrameBounds(HWND hwnd, bool* usedDwm) {
    RECT frame{};
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                             &frame, sizeof(frame));
    if (SUCCEEDED(hr)) {
        if (usedDwm) {
            *usedDwm = true;
        }
        return frame;
    }

    // Windows that DWM does not track, such as the desktop window, land here.
    if (usedDwm) {
        *usedDwm = false;
    }
    GetWindowRect(hwnd, &frame);
    return frame;
}

void LogDesktopGeometry(const wchar_t* reason) {
    const DesktopGeometry desktop = QueryDesktop();

    SC_LOG(L"--- monitor layout (%s) ---", reason);
    SC_LOG(L"virtual desktop: origin=(%ld,%ld) size=%ldx%ld  right=%ld bottom=%ld",
           desktop.bounds.left, desktop.bounds.top, desktop.Width(), desktop.Height(),
           desktop.bounds.right, desktop.bounds.bottom);

    for (const MonitorGeometry& m : desktop.monitors) {
        SC_LOG(L"  %s%-12s dpi=%u(%u%%)  rcMonitor=(%ld,%ld,%ld,%ld) %ldx%ld",
               m.primary ? L"* " : L"  ", m.device.c_str(), m.dpi, m.ScalePercent(),
               m.monitor.left, m.monitor.top, m.monitor.right, m.monitor.bottom,
               m.MonitorWidth(), m.MonitorHeight());
        SC_LOG(L"                               rcWork   =(%ld,%ld,%ld,%ld) %ldx%ld",
               m.work.left, m.work.top, m.work.right, m.work.bottom,
               m.WorkWidth(), m.WorkHeight());
    }

    // Record whether the two conditions the test plan calls for hold on this
    // machine. If neither does, the mixed-DPI and negative-origin paths are
    // never exercised.
    SC_LOG(L"test conditions: mixed DPI=%s, negative origin=%s",
           desktop.HasMixedDpi() ? L"yes" : L"no (path unexercised)",
           desktop.HasNegativeOrigin() ? L"yes" : L"no (path unexercised)");
}

}  // namespace sc
