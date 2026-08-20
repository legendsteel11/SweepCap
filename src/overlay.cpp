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

// Size readout.
constexpr COLORREF kLabelBack = RGB(24, 24, 28);
constexpr COLORREF kLabelText = RGB(255, 255, 255);
constexpr int kLabelPaddingX = 8;
constexpr int kLabelPaddingY = 4;
constexpr int kLabelGap = 8;

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

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
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

    const RECT& bounds = frame.Bounds();
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top,
                 static_cast<int>(bounds.right - bounds.left),
                 static_cast<int>(bounds.bottom - bounds.top),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);  // paint the first frame now; a late one reads as a flash
    return true;
}

void Overlay::Hide() {
    if (!hwnd_) {
        return;
    }
    visible_ = false;
    frame_ = nullptr;
    ShowWindow(hwnd_, SW_HIDE);
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
    constexpr int kMargin = 80;
    const RECT changed[2] = {before, after};
    for (const RECT& r : changed) {
        RECT client = ToClient(r);
        InflateRect(&client, kMargin, kMargin);
        InvalidateRect(hwnd_, &client, FALSE);
    }
}

void Overlay::SetSelection(const RECT& selection) {
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
    if (EqualRect(&normalized, &selection_)) {
        return;
    }
    const RECT before = selection_;
    selection_ = normalized;
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
                label = RECT{sel.left, sel.bottom + kLabelGap, sel.left + boxWidth,
                             sel.bottom + kLabelGap + boxHeight};

                // Fold it back inside when it would leave the screen.
                const RECT& bounds = frame_->Bounds();
                const LONG clientBottom = bounds.bottom - bounds.top;
                const LONG clientRight = bounds.right - bounds.left;
                if (label.bottom > clientBottom) {
                    label.top = sel.bottom - kLabelGap - boxHeight;
                    label.bottom = sel.bottom - kLabelGap;
                }
                if (label.right > clientRight) {
                    const LONG shift = label.right - clientRight;
                    label.left -= shift;
                    label.right -= shift;
                }
                if (label.left < 0) {
                    label.right -= label.left;
                    label.left = 0;
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
        for (int i = 0; i < count; ++i) {
            FillRectColor(dc, pieces[i], kBorderInner);
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
