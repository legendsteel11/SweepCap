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
        return TRUE;  // 한 대를 못 읽어도 나머지는 계속 센다.
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

    // 가상 데스크탑은 SM_* 로 얻는다. 원점이 음수일 수 있다.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    desktop.bounds.left = x;
    desktop.bounds.top = y;
    desktop.bounds.right = x + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bounds.bottom = y + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    EnumDisplayMonitors(nullptr, nullptr, EnumProc,
                        reinterpret_cast<LPARAM>(&desktop.monitors));

    // 주 모니터를 앞으로, 나머지는 좌상단 순으로. 로그를 읽기 쉽게 하려는 것뿐이다.
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

    // DWM이 추적하지 않는 창(데스크탑 창 등)은 여기로 온다.
    if (usedDwm) {
        *usedDwm = false;
    }
    GetWindowRect(hwnd, &frame);
    return frame;
}

void LogDesktopGeometry(const wchar_t* reason) {
    const DesktopGeometry desktop = QueryDesktop();

    SC_LOG(L"--- 모니터 레이아웃 (%s) ---", reason);
    SC_LOG(L"가상 데스크탑: origin=(%ld,%ld) size=%ldx%ld  right=%ld bottom=%ld",
           desktop.bounds.left, desktop.bounds.top, desktop.Width(), desktop.Height(),
           desktop.bounds.right, desktop.bounds.bottom);

    for (const MonitorGeometry& m : desktop.monitors) {
        SC_LOG(L"  %s%-12s dpi=%u(%u%%)  rcMonitor=(%ld,%ld,%ld,%ld) %ldx%ld",
               m.primary ? L"* " : L"  ", m.device.c_str(), m.dpi, m.ScalePercent(),
               m.monitor.left, m.monitor.top, m.monitor.right, m.monitor.bottom,
               m.MonitorWidth(), m.MonitorHeight());
        SC_LOG(L"                               " L"rcWork   =(%ld,%ld,%ld,%ld) %ldx%ld",
               m.work.left, m.work.top, m.work.right, m.work.bottom,
               m.WorkWidth(), m.WorkHeight());
    }

    // CLAUDE.md "테스트 환경"이 요구하는 두 조건이 지금 구성에서 성립하는지 남긴다.
    // 둘 다 아니면 혼합 DPI와 음수 좌표 경로를 한 번도 안 밟고 지나간다.
    SC_LOG(L"검증 조건: 혼합 DPI=%s, 음수 원점=%s",
           desktop.HasMixedDpi() ? L"예" : L"아니오 (미검증 경로)",
           desktop.HasNegativeOrigin() ? L"예" : L"아니오 (미검증 경로)");
}

}  // namespace sc
