#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 좌표계 규약 (CLAUDE.md "좌표계 규약")
//
//   - 내부 좌표는 전부 물리 픽셀이다.
//   - 원점은 가상 데스크탑 좌상단이며 X와 Y가 음수일 수 있다.
//     (주 모니터 왼쪽이나 위에 보조 모니터가 있는 경우)
//   - 창의 사각형은 GetWindowRect가 아니라 DWMWA_EXTENDED_FRAME_BOUNDS로 얻는다.
//   - 격자와 배치의 기준 영역은 rcMonitor가 아니라 rcWork다.
//
// 이 규약은 전 코드에 퍼지므로 중간에 바꾸면 전부 재검증해야 한다.

namespace sc {

struct MonitorGeometry {
    std::wstring device;
    RECT monitor{};   // 모니터 전체 영역
    RECT work{};      // 작업 영역. 격자와 배치는 이쪽을 쓴다.
    UINT dpi = 96;    // 유효 DPI. 96이 100%.
    bool primary = false;

    LONG MonitorWidth() const { return monitor.right - monitor.left; }
    LONG MonitorHeight() const { return monitor.bottom - monitor.top; }
    LONG WorkWidth() const { return work.right - work.left; }
    LONG WorkHeight() const { return work.bottom - work.top; }
    UINT ScalePercent() const { return dpi * 100 / 96; }
};

struct DesktopGeometry {
    RECT bounds{};    // 가상 데스크탑. left와 top이 음수일 수 있다.
    std::vector<MonitorGeometry> monitors;

    LONG Width() const { return bounds.right - bounds.left; }
    LONG Height() const { return bounds.bottom - bounds.top; }
    bool HasMixedDpi() const;
    bool HasNegativeOrigin() const { return bounds.left < 0 || bounds.top < 0; }
};

// 현재 모니터 구성을 읽는다. 호출 프로세스가 Per-Monitor V2여야 물리 픽셀이 나온다.
DesktopGeometry QueryDesktop();

// 창의 보이는 사각형. Windows 11 창은 보이는 테두리 밖에 리사이즈 여백이 있어서
// GetWindowRect를 그대로 쓰면 배경이 딸려온다.
// usedDwm에는 DWM 값을 썼는지(true) GetWindowRect로 물러났는지(false)가 들어간다.
RECT WindowFrameBounds(HWND hwnd, bool* usedDwm = nullptr);

// 디버그 계측: 현재 레이아웃을 로그에 덤프한다. reason은 무엇 때문에 찍었는지.
void LogDesktopGeometry(const wchar_t* reason);

}  // namespace sc
