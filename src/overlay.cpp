#include "overlay.h"

#include <shellscalingapi.h>

#include <algorithm>

#include "capture.h"
#include "log.h"

namespace sc {
namespace {

constexpr wchar_t kOverlayClass[] = L"SweepCap.Overlay";

// 선택 영역 밖을 덮는 어둠의 진하기. 0~255.
constexpr BYTE kDimAlpha = 110;

// 선택 테두리. 바깥 검정 1px, 안쪽 흰색 1px이라 어떤 배경에서도 보인다.
constexpr COLORREF kBorderInner = RGB(255, 255, 255);
constexpr COLORREF kBorderOuter = RGB(0, 0, 0);

// 크기 표시 상자.
constexpr COLORREF kLabelBack = RGB(24, 24, 28);
constexpr COLORREF kLabelText = RGB(255, 255, 255);
constexpr int kLabelPadding = 6;
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

// 두께 thickness의 사각형 테두리. FrameRect는 항상 1px이라 직접 그린다.
void DrawBorder(HDC dc, const RECT& r, COLORREF color, int thickness) {
    const RECT top{r.left, r.top, r.right, r.top + thickness};
    const RECT bottom{r.left, r.bottom - thickness, r.right, r.bottom};
    const RECT left{r.left, r.top + thickness, r.left + thickness, r.bottom - thickness};
    const RECT right{r.right - thickness, r.top + thickness, r.right, r.bottom - thickness};
    FillRectColor(dc, top, color);
    FillRectColor(dc, bottom, color);
    FillRectColor(dc, left, color);
    FillRectColor(dc, right, color);
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
            return 1;  // 전부 직접 그린다. 지우면 깜빡인다.

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                self->Paint(dc, ps.rcPaint);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        // 활성화되지 않는 창이다. 밑에 있던 창이 활성 상태로 남아야
        // 캡처가 끝난 뒤 사용자가 하던 일을 이어갈 수 있다.
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
        wc.hbrBackground = nullptr;
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

    // 어둡게 깔 때 늘려 쓸 1x1 검정 비트맵.
    wil::unique_hdc_window screen{wil::window_dc{GetDC(nullptr), nullptr}};
    dimDc_.reset(CreateCompatibleDC(screen.get()));
    if (dimDc_) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 1;
        info.bmiHeader.biHeight = -1;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dimBitmap_.reset(
            CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (dimBitmap_ && bits != nullptr) {
            *static_cast<uint32_t*>(bits) = 0xFF000000u;  // 검정, 불투명
            dimSelection_ = wil::SelectObject(dimDc_.get(), dimBitmap_.get());
        }
    }
    return true;
}

bool Overlay::Show(const FrozenFrame& frame, POINT anchor) {
    if (!EnsureWindow()) {
        return false;
    }

    frame_ = &frame;
    selection_ = RECT{anchor.x, anchor.y, anchor.x, anchor.y};

    // 커서가 있는 모니터의 DPI로 글꼴을 만든다. 창이 여러 모니터에 걸쳐 있으면
    // 정답이 하나가 아니므로 드래그 시작 지점 기준으로 정한다.
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
    UpdateWindow(hwnd_);  // 첫 화면은 즉시 그린다. 늦으면 깜빡임으로 보인다.
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
    // 테두리와 크기 표시가 사각형 밖으로 나가므로 넉넉히 무효화한다.
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

void Overlay::Paint(HDC dc, const RECT& dirty) const {
    if (!frame_ || !frame_->Valid()) {
        return;
    }

    const int dirtyWidth = static_cast<int>(dirty.right - dirty.left);
    const int dirtyHeight = static_cast<int>(dirty.bottom - dirty.top);
    if (dirtyWidth <= 0 || dirtyHeight <= 0) {
        return;
    }

    // 1. 정지 화면을 그대로 깐다.
    BitBlt(dc, dirty.left, dirty.top, dirtyWidth, dirtyHeight, frame_->Dc(), dirty.left,
           dirty.top, SRCCOPY);

    const RECT selection = ToClient(selection_);

    // 2. 선택 영역 밖을 어둡게 한다. 무효 영역에서 선택 영역을 뺀 조각들이다.
    if (dimDc_) {
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = kDimAlpha;

        RECT overlap{};
        RECT pieces[4]{};
        int pieceCount = 0;
        if (IntersectRect(&overlap, &dirty, &selection)) {
            pieces[pieceCount++] = RECT{dirty.left, dirty.top, dirty.right, overlap.top};
            pieces[pieceCount++] = RECT{dirty.left, overlap.bottom, dirty.right, dirty.bottom};
            pieces[pieceCount++] = RECT{dirty.left, overlap.top, overlap.left, overlap.bottom};
            pieces[pieceCount++] = RECT{overlap.right, overlap.top, dirty.right, overlap.bottom};
        } else {
            pieces[pieceCount++] = dirty;
        }
        for (int i = 0; i < pieceCount; ++i) {
            const RECT& piece = pieces[i];
            const int w = static_cast<int>(piece.right - piece.left);
            const int h = static_cast<int>(piece.bottom - piece.top);
            if (w > 0 && h > 0) {
                AlphaBlend(dc, piece.left, piece.top, w, h, dimDc_.get(), 0, 0, 1, 1, blend);
            }
        }
    }

    if (selection.right <= selection.left || selection.bottom <= selection.top) {
        return;
    }

    // 3. 테두리.
    RECT outer = selection;
    InflateRect(&outer, 2, 2);
    DrawBorder(dc, outer, kBorderOuter, 1);
    RECT inner = selection;
    InflateRect(&inner, 1, 1);
    DrawBorder(dc, inner, kBorderInner, 1);

    // 4. 크기 표시.
    if (!labelFont_) {
        return;
    }
    wchar_t text[64];
    if (swprintf_s(text, L"%ld x %ld", selection.right - selection.left,
                   selection.bottom - selection.top) < 0) {
        return;
    }

    auto fontScope = wil::SelectObject(dc, labelFont_.get());
    RECT measure{0, 0, 0, 0};
    DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

    const LONG boxWidth = (measure.right - measure.left) + kLabelPadding * 2;
    const LONG boxHeight = (measure.bottom - measure.top) + kLabelPadding;

    // 기본은 사각형 아래 왼쪽 맞춤. 화면 밖으로 나가면 안쪽으로 접는다.
    RECT box{selection.left, selection.bottom + kLabelGap, selection.left + boxWidth,
             selection.bottom + kLabelGap + boxHeight};
    const RECT& bounds = frame_->Bounds();
    const LONG clientBottom = bounds.bottom - bounds.top;
    const LONG clientRight = bounds.right - bounds.left;
    if (box.bottom > clientBottom) {
        box.top = selection.bottom - kLabelGap - boxHeight;
        box.bottom = selection.bottom - kLabelGap;
    }
    if (box.right > clientRight) {
        const LONG shift = box.right - clientRight;
        box.left -= shift;
        box.right -= shift;
    }
    if (box.left < 0) {
        box.right -= box.left;
        box.left = 0;
    }

    FillRectColor(dc, box, kLabelBack);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldText = SetTextColor(dc, kLabelText);
    DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, oldText);
    SetBkMode(dc, oldMode);
}

}  // namespace sc
