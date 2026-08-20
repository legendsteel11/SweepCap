#include "overlay.h"

#include <shellscalingapi.h>

#include <algorithm>

#include "capture.h"
#include "log.h"

namespace sc {
namespace {

constexpr wchar_t kOverlayClass[] = L"SweepCap.Overlay";

// 선택 테두리. 바깥 검정 1px, 안쪽 흰색 1px이라 어떤 배경에서도 보인다.
constexpr COLORREF kBorderInner = RGB(255, 255, 255);
constexpr COLORREF kBorderOuter = RGB(0, 0, 0);

// 크기 표시 상자.
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


// a에서 b를 뺀 영역을 최대 네 조각의 사각형으로 나눈다. 조각끼리 겹치지 않는다.
// 이것이 "픽셀마다 한 번만 쓴다"를 지키는 도구다.
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
            return 1;  // 전부 직접 그린다. 지우면 깜빡인다.

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (self != nullptr) {
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
        // 배경은 직접 그리지만, 창이 처음 보이고 첫 WM_PAINT가 오기 전까지
        // 시스템이 한 번 칠하는 구간이 있다. 이때 흰색이 스치지 않게 검정으로 둔다.
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

void Overlay::Paint(HDC dc, const RECT& dirty) {
    if (!frame_ || !frame_->Valid()) {
        return;
    }
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
        return;
    }

    // 어두운 사본이 없으면(만들기 실패) 원본으로 대신한다.
    // 어둡게는 안 되지만 선택은 그대로 할 수 있다.
    HDC dimSource = frame_->DimDc() != nullptr ? frame_->DimDc() : frame_->Dc();

    const RECT sel = ToClient(selection_);
    const bool hasSelection = sel.right > sel.left && sel.bottom > sel.top;

    // 테두리는 선택 영역 바깥에 붙는다. 바깥 1px 검정, 안쪽 1px 흰색.
    RECT ringOuter = sel;
    RECT ringMid = sel;
    if (hasSelection) {
        InflateRect(&ringOuter, 2, 2);
        InflateRect(&ringMid, 1, 1);
    }

    // 크기 표시 상자를 먼저 계산한다. 밑칠에서 이 자리를 빼야 하기 때문이다.
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

                // 화면 밖으로 나가면 안쪽으로 접는다.
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

    // 여기부터가 "픽셀마다 한 번만 쓴다"를 지키는 부분이다.
    //
    // 밑칠(어두운 사본 / 원본 / 테두리)은 서로 겹치지 않는 조각으로 나눠 칠하고,
    // 크기 표시 자리는 클립에서 아예 빼 둔다. 그래서 어떤 픽셀도 두 번 칠해지지
    // 않는다. 두 번 칠하면 그 사이에 모니터가 갱신될 때 중간 상태가 보이고,
    // 그것이 테두리가 흔들려 보이던 원인이었다.
    const int savedDc = SaveDC(dc);
    if (hasLabel) {
        ExcludeClipRect(dc, label.left, label.top, label.right, label.bottom);
    }

    RECT pieces[4]{};
    int count = 0;

    // 1. 테두리 바깥: 어두운 사본
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
        // 2. 테두리 검정 1px
        count = SubtractRect(ringOuter, ringMid, pieces);
        for (int i = 0; i < count; ++i) {
            FillRectColor(dc, pieces[i], kBorderOuter);
        }

        // 3. 테두리 흰색 1px
        count = SubtractRect(ringMid, sel, pieces);
        for (int i = 0; i < count; ++i) {
            FillRectColor(dc, pieces[i], kBorderInner);
        }

        // 4. 선택 영역 안: 원본
        RECT inside{};
        if (IntersectRect(&inside, &dirty, &sel)) {
            BitBlt(dc, inside.left, inside.top, static_cast<int>(inside.right - inside.left),
                   static_cast<int>(inside.bottom - inside.top), frame_->Dc(), inside.left,
                   inside.top, SRCCOPY);
        }
    }

    RestoreDC(dc, savedDc);

    // 5. 크기 표시. 배경과 글자를 한 번에 쓴다.
    //    배경을 칠한 뒤 글자를 얹으면 같은 문제가 생긴다.
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
