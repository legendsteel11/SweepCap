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
class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // frame은 Hide()를 부를 때까지 살아 있어야 한다.
    bool Show(const FrozenFrame& frame, POINT anchor);
    void SetSelection(const RECT& selection);  // 가상 데스크탑 좌표
    void Hide();
    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow();
    void Paint(HDC dc, const RECT& dirty) const;
    void InvalidateForSelection(const RECT& before, const RECT& after) const;
    RECT ToClient(const RECT& virtualRect) const;

    HWND hwnd_ = nullptr;
    const FrozenFrame* frame_ = nullptr;
    RECT selection_{};
    bool visible_ = false;

    // 어둡게 깔 때 쓰는 1x1 검정 비트맵. AlphaBlend로 늘려 그린다.
    wil::unique_hdc dimDc_;
    wil::unique_hbitmap dimBitmap_;
    wil::unique_select_object dimSelection_;

    wil::unique_hfont labelFont_;
};

}  // namespace sc
