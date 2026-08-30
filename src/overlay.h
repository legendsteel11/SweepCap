#pragma once

#include <windows.h>
#include <wil/resource.h>

namespace sc {

class FrozenFrame;

// The frozen-frame overlay.
//
// A window covering the whole virtual desktop. What the user sees is the still
// image this window paints; cropping happens against the FrozenFrame bitmap, so
// the selection rectangle and the size readout never appear in the result.
//
// Display only. The hook tracks the drag.
//
// Painting rule: every pixel is written exactly once. Inside the selection the
// original is laid down, outside it the darkened copy, and the border occupies
// its own disjoint bands. Writing a pixel twice lets a display refresh land
// between the two writes and show the intermediate state, which reads as
// flicker along the moving edges.
class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Creates the window ahead of time so Show() has less to do.
    void Prepare();

    // frame must outlive the call to Hide().
    //
    // The selection is supplied rather than started empty. The overlay goes up
    // only once the selection is decided, and an empty one paints the whole
    // screen as "outside", so starting empty would dim everything for a frame.
    bool Show(const FrozenFrame& frame, POINT anchor, const RECT& selection, bool windowMode,
              POINT cursor);

    // selection is in virtual desktop coordinates. windowMode tints the border
    // so it is obvious that a whole window is selected rather than a rectangle
    // the user dragged.
    void SetSelection(const RECT& selection, bool windowMode, POINT cursor);
    void Hide();
    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow();
    void Paint(HDC dc, const RECT& dirty);
    void StepFade();
    void InvalidateForSelection(const RECT& before, const RECT& after) const;
    RECT ToClient(const RECT& virtualRect) const;
    POINT ToClientPoint(POINT virtualPoint) const;

    HWND hwnd_ = nullptr;
    const FrozenFrame* frame_ = nullptr;
    RECT selection_{};
    bool visible_ = false;
    bool windowMode_ = false;
    POINT cursor_{};
    int fadeStep_ = 0;

    wil::unique_hfont labelFont_;
};

}  // namespace sc
