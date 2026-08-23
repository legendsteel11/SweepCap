#include "overlay.h"

#include <shellscalingapi.h>

#include <algorithm>

#include "capture.h"
#include "log.h"

namespace sc {
namespace {

constexpr wchar_t kOverlayClass[] = L"SweepCap.Overlay";

// Selection border: 1px black outside, 1px white inside, so it stays visible
// against any background.
constexpr COLORREF kBorderInner = RGB(255, 255, 255);
constexpr COLORREF kBorderOuter = RGB(0, 0, 0);

// A whole-window selection uses the accent colour instead, so the change of
// mode is visible without reading the size readout.
constexpr COLORREF kBorderWindow = RGB(96, 156, 255);

// Size readout.
constexpr COLORREF kLabelBack = RGB(24, 24, 28);
constexpr COLORREF kLabelText = RGB(255, 255, 255);
constexpr int kLabelPaddingX = 8;
constexpr int kLabelPaddingY = 4;
constexpr int kLabelGap = 8;

// Fade-in of the whole overlay.
//
// Appearing at full strength in one frame reads as a flash, because the change
// is large and instant. Stepping the window's opacity instead costs one API
// call per step and no repainting at all.
//
// Kept short: the drag is already under way, and the selection is a zero-sized
// rectangle for the first moments anyway, so there is nothing to see yet.
constexpr UINT_PTR kFadeTimerId = 1;
constexpr UINT kFadeStepMs = 16;   // roughly one display refresh
constexpr int kFadeSteps = 6;      // about 96 ms in total
constexpr BYTE kFadeStart = 40;    // not zero: something has to appear at once

bool g_classRegistered = false;

void FillRectColor(HDC dc, const RECT& r, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) {
        return;
    }
    const COLORREF old = SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    SetBkColor(dc, old);
}

// Splits a minus b into at most four non-overlapping rectangles.
// This is the tool that keeps painting to one write per pixel.
int SubtractRect(const RECT& a, const RECT& b, RECT* out) {
    if (a.right <= a.left || a.bottom <= a.top) {
        return 0;
    }
    RECT overlap{};
    if (!IntersectRect(&overlap, &a, &b)) {
        out[0] = a;
        return 1;
    }
    int n = 0;
    if (overlap.top > a.top) {
        out[n++] = RECT{a.left, a.top, a.right, overlap.top};
    }
    if (overlap.bottom < a.bottom) {
        out[n++] = RECT{a.left, overlap.bottom, a.right, a.bottom};
    }
    if (overlap.left > a.left) {
        out[n++] = RECT{a.left, overlap.top, overlap.left, overlap.bottom};
    }
    if (overlap.right < a.right) {
        out[n++] = RECT{overlap.right, overlap.top, a.right, overlap.bottom};
    }
    return n;
}

UINT DpiForPoint(POINT pt) {
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return dpiX;
    }
    return 96;
}

}  // namespace

Overlay::~Overlay() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }

        case WM_ERASEBKGND:
            return 1;  // everything is painted explicitly; erasing would flicker

        case WM_TIMER:
            if (wparam == kFadeTimerId && self != nullptr) {
                self->StepFade();
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (self != nullptr) {
                self->Paint(dc, ps.rcPaint);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        // This window never activates, so whatever was in front stays active
        // and the user can carry on where they left off after the capture.
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool Overlay::EnsureWindow() {
    if (hwnd_) {
        return true;
    }

    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Overlay::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        // The background is painted explicitly, but there is a window between
        // the window becoming visible and the first WM_PAINT during which the
        // system fills it. Black keeps white from flashing through there.
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (RegisterClassExW(&wc) == 0) {
            SC_LOG(L"[오버레이] RegisterClassEx 실패 err=%lu", GetLastError());
            return false;
        }
        g_classRegistered = true;
    }

    // WS_EX_LAYERED so the dimming can be faded in. The window still paints
    // the same pixels; only its opacity changes, and DWM does that blend on
    // the GPU. Compositing the fade ourselves would mean rewriting the whole
    // frame once per step, which is the approach measured at 34 ms and
    // abandoned.
    //
    // Not WS_EX_TRANSPARENT: the overlay is what shows the cross cursor, and
    // it only receives WM_SETCURSOR while it takes part in hit testing. Window
    // picking is safe regardless, because every pick runs before the overlay
    // is shown.
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kOverlayClass,
        L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        SC_LOG(L"[오버레이] CreateWindowEx 실패 err=%lu", GetLastError());
        return false;
    }
    return true;
}

void Overlay::Prepare() { EnsureWindow(); }

bool Overlay::Show(const FrozenFrame& frame, POINT anchor) {
    if (!EnsureWindow()) {
        return false;
    }

    frame_ = &frame;
    selection_ = RECT{anchor.x, anchor.y, anchor.x, anchor.y};
    windowMode_ = false;

    // Build the font for the DPI of the monitor under the cursor. A window
    // spanning monitors of different DPI has no single right answer, so the
    // drag origin decides.
    const UINT dpi = DpiForPoint(anchor);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(12, static_cast<int>(dpi), 72);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    labelFont_.reset(CreateFontIndirectW(&lf));

    // Set the starting opacity before the window is shown, so the first frame
    // the user sees is already the faded one.
    fadeStep_ = 0;
    SetLayeredWindowAttributes(hwnd_, 0, kFadeStart, LWA_ALPHA);

    const RECT& bounds = frame.Bounds();
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top,
                 static_cast<int>(bounds.right - bounds.left),
                 static_cast<int>(bounds.bottom - bounds.top),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);  // paint the first frame now; a late one reads as a flash
    SetTimer(hwnd_, kFadeTimerId, kFadeStepMs, nullptr);
    return true;
}

// One step of the fade. The window content does not change, so this never
// invalidates anything.
void Overlay::StepFade() {
    if (!hwnd_) {
        return;
    }
    ++fadeStep_;
    if (fadeStep_ >= kFadeSteps) {
        KillTimer(hwnd_, kFadeTimerId);
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        return;
    }
    const int alpha =
        kFadeStart + (255 - kFadeStart) * fadeStep_ / kFadeSteps;
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
}

void Overlay::Hide() {
    if (!hwnd_) {
        return;
    }
    KillTimer(hwnd_, kFadeTimerId);
    fadeStep_ = kFadeSteps;
    visible_ = false;
    frame_ = nullptr;
    ShowWindow(hwnd_, SW_HIDE);
}

POINT Overlay::ToClientPoint(POINT virtualPoint) const {
    if (!frame_) {
        return POINT{};
    }
    const RECT& b = frame_->Bounds();
    return POINT{virtualPoint.x - b.left, virtualPoint.y - b.top};
}

RECT Overlay::ToClient(const RECT& virtualRect) const {
    if (!frame_) {
        return RECT{};
    }
    const RECT& b = frame_->Bounds();
    return RECT{virtualRect.left - b.left, virtualRect.top - b.top,
                virtualRect.right - b.left, virtualRect.bottom - b.top};
}

void Overlay::InvalidateForSelection(const RECT& before, const RECT& after) const {
    if (!hwnd_) {
        return;
    }
    // The border and the size readout extend past the rectangle, so invalidate
    // with room to spare.
    constexpr int kMargin = 200;
    const RECT changed[2] = {before, after};
    for (const RECT& r : changed) {
        RECT client = ToClient(r);
        InflateRect(&client, kMargin, kMargin);
        InvalidateRect(hwnd_, &client, FALSE);
    }
}

void Overlay::SetSelection(const RECT& selection, bool windowMode, POINT cursor) {
    cursor_ = cursor;
    if (!visible_ || !hwnd_) {
        return;
    }
    RECT normalized = selection;
    if (normalized.left > normalized.right) {
        std::swap(normalized.left, normalized.right);
    }
    if (normalized.top > normalized.bottom) {
        std::swap(normalized.top, normalized.bottom);
    }
    if (EqualRect(&normalized, &selection_) && windowMode == windowMode_) {
        return;
    }
    const RECT before = selection_;
    selection_ = normalized;
    windowMode_ = windowMode;
    InvalidateForSelection(before, selection_);
    UpdateWindow(hwnd_);
}

void Overlay::Paint(HDC dc, const RECT& dirty) {
    if (!frame_ || !frame_->Valid()) {
        return;
    }
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
        return;
    }

    // If building the darkened copy failed, fall back to the original. Nothing
    // gets dimmed, but the selection still works.
    HDC dimSource = frame_->DimDc() != nullptr ? frame_->DimDc() : frame_->Dc();

    const RECT sel = ToClient(selection_);
    const bool hasSelection = sel.right > sel.left && sel.bottom > sel.top;

    // The border sits outside the selection: 1px black then 1px white.
    RECT ringOuter = sel;
    RECT ringMid = sel;
    if (hasSelection) {
        InflateRect(&ringOuter, 2, 2);
        InflateRect(&ringMid, 1, 1);
    }

    // The size readout is measured first because its area has to be excluded
    // from everything painted underneath.
    wchar_t text[64];
    int textLength = 0;
    RECT label{};
    bool hasLabel = false;
    wil::unique_select_object fontScope;
    if (hasSelection && labelFont_) {
        textLength = swprintf_s(text, L"%ld x %ld", sel.right - sel.left, sel.bottom - sel.top);
        if (textLength > 0) {
            fontScope = wil::SelectObject(dc, labelFont_.get());
            SIZE textSize{};
            if (GetTextExtentPoint32W(dc, text, textLength, &textSize)) {
                const LONG boxWidth = textSize.cx + kLabelPaddingX * 2;
                const LONG boxHeight = textSize.cy + kLabelPaddingY * 2;
                // The readout follows the cursor rather than sitting at a fixed
                // corner of the selection. On a large screen the dragged corner
                // and a fixed corner can be most of a metre apart, and reading
                // the size then means looking away from the work.
                //
                // It goes on the far side of the cursor from the selection, so
                // it never covers the area being chosen: dragging down-right
                // puts it below-right, dragging up-left puts it above-left.
                const POINT cursor = ToClientPoint(cursor_);
                const LONG midX = (sel.left + sel.right) / 2;
                const LONG midY = (sel.top + sel.bottom) / 2;
                const LONG gap = kLabelGap * 2;
                label.left = cursor.x >= midX ? cursor.x + gap : cursor.x - gap - boxWidth;
                label.top = cursor.y >= midY ? cursor.y + gap : cursor.y - gap - boxHeight;
                label.right = label.left + boxWidth;
                label.bottom = label.top + boxHeight;

                // Fold it back inside when it would leave the screen.
                const RECT& bounds = frame_->Bounds();
                const LONG clientBottom = bounds.bottom - bounds.top;
                const LONG clientRight = bounds.right - bounds.left;
                if (label.right > clientRight) {
                    const LONG shift = label.right - clientRight;
                    label.left -= shift;
                    label.right -= shift;
                }
                if (label.left < 0) {
                    label.right -= label.left;
                    label.left = 0;
                }
                if (label.bottom > clientBottom) {
                    const LONG shift = label.bottom - clientBottom;
                    label.top -= shift;
                    label.bottom -= shift;
                }
                if (label.top < 0) {
                    label.bottom -= label.top;
                    label.top = 0;
                }
                hasLabel = true;
            }
        }
    }

    // This is where the one-write-per-pixel rule is enforced.
    //
    // The underlying passes - darkened copy, border, original - are split into
    // non-overlapping rectangles, and the size readout is clipped out entirely.
    // Painting a pixel twice lets a display refresh land between the two writes
    // and show the intermediate state, which is what made the border shimmer.
    const int savedDc = SaveDC(dc);
    if (hasLabel) {
        ExcludeClipRect(dc, label.left, label.top, label.right, label.bottom);
    }

    RECT pieces[4]{};
    int count = 0;

    // 1. Outside the border: the darkened copy.
    count = hasSelection ? SubtractRect(dirty, ringOuter, pieces) : 0;
    if (!hasSelection) {
        pieces[count++] = dirty;
    }
    for (int i = 0; i < count; ++i) {
        const RECT& r = pieces[i];
        BitBlt(dc, r.left, r.top, static_cast<int>(r.right - r.left),
               static_cast<int>(r.bottom - r.top), dimSource, r.left, r.top, SRCCOPY);
    }

    if (hasSelection) {
        // 2. Border, outer 1px.
        count = SubtractRect(ringOuter, ringMid, pieces);
        for (int i = 0; i < count; ++i) {
            FillRectColor(dc, pieces[i], kBorderOuter);
        }

        // 3. Border, inner 1px.
        count = SubtractRect(ringMid, sel, pieces);
        const COLORREF innerColor = windowMode_ ? kBorderWindow : kBorderInner;
        for (int i = 0; i < count; ++i) {
            FillRectColor(dc, pieces[i], innerColor);
        }

        // 4. Inside the selection: the original.
        RECT inside{};
        if (IntersectRect(&inside, &dirty, &sel)) {
            BitBlt(dc, inside.left, inside.top, static_cast<int>(inside.right - inside.left),
                   static_cast<int>(inside.bottom - inside.top), frame_->Dc(), inside.left,
                   inside.top, SRCCOPY);
        }
    }

    RestoreDC(dc, savedDc);

    // 5. Size readout. ETO_OPAQUE writes background and glyphs in one call;
    //    filling the box and then drawing text would be two writes again.
    if (hasLabel) {
        const COLORREF oldBk = SetBkColor(dc, kLabelBack);
        const COLORREF oldText = SetTextColor(dc, kLabelText);
        const int oldMode = SetBkMode(dc, OPAQUE);
        ExtTextOutW(dc, label.left + kLabelPaddingX, label.top + kLabelPaddingY, ETO_OPAQUE,
                    &label, text, static_cast<UINT>(textLength), nullptr);
        SetBkMode(dc, oldMode);
        SetTextColor(dc, oldText);
        SetBkColor(dc, oldBk);
    }
}

}  // namespace sc
