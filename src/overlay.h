#pragma once

#include <windows.h>
#include <wil/resource.h>

namespace sc {

class FrozenFrame;

// 프리즈 프레임 오버레이.
//
// 가상 데스크탑 전체를 덮고 떠 있는 창이다. 화면에 보이는 것은 이 창이 그린
// 정지 화면이고, 실제 잘라내기는 FrozenFrame의 비트맵에서 한다.
// 그래서 선택 사각형이나 크기 표시가 결과물에 찍히지 않는다.
//
// 표시 전용이다. 드래그 추적은 훅이 한다.
//
// 그리기 규칙: 픽셀마다 딱 한 번만 쓴다.
//   선택 영역 안 -> 원본, 밖 -> 어둡게 만든 사본을 그대로 깐다.
// 밝게 깔고 그 위를 덮는 방식은 그 사이에 모니터가 갱신되면 밝은 상태가 보인다.
// 실제로 드래그 중 번쩍임으로 나타났다.
class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // 미리 준비한다. 창을 만들어 두는 것이 전부다.
    void Prepare();

    // frame은 Hide()를 부를 때까지 살아 있어야 한다.
    bool Show(const FrozenFrame& frame, POINT anchor);
    void SetSelection(const RECT& selection);  // 가상 데스크탑 좌표
    void Hide();
    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow();
    void Paint(HDC dc, const RECT& dirty);
    void InvalidateForSelection(const RECT& before, const RECT& after) const;
    RECT ToClient(const RECT& virtualRect) const;

    HWND hwnd_ = nullptr;
    const FrozenFrame* frame_ = nullptr;
    RECT selection_{};
    bool visible_ = false;

    wil::unique_hfont labelFont_;
};

}  // namespace sc
