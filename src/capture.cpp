#include "capture.h"

#include <algorithm>

#include "coords.h"
#include "log.h"

namespace sc {
namespace {

// 계측용 경과 시간.
class Stopwatch {
public:
    Stopwatch() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_);
    }
    double ElapsedMs() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 /
               static_cast<double>(freq_.QuadPart);
    }

private:
    LARGE_INTEGER freq_{};
    LARGE_INTEGER start_{};
};

}  // namespace

void FrozenFrame::Reset() {
    selection_.reset();
    bitmap_.reset();
    memDc_.reset();
    pixels_ = nullptr;
    bounds_ = RECT{};
    grabbedAt_ = 0;
}

bool FrozenFrame::GrabPixels() {
    Reset();

    const Stopwatch watch;

    const DesktopGeometry desktop = QueryDesktop();
    const RECT bounds = desktop.bounds;
    const int width = static_cast<int>(bounds.right - bounds.left);
    const int height = static_cast<int>(bounds.bottom - bounds.top);
    if (width <= 0 || height <= 0) {
        SC_LOG(L"[캡처] 가상 데스크탑 크기가 이상하다: %dx%d", width, height);
        return false;
    }

    wil::unique_hdc_window screenDc{wil::window_dc{GetDC(nullptr), nullptr}};
    if (!screenDc) {
        SC_LOG(L"[캡처] GetDC(nullptr) 실패 err=%lu", GetLastError());
        return false;
    }

    // top-down DIB. biHeight를 음수로 주면 첫 행이 화면 맨 윗줄이 되어
    // 잘라내기와 인코딩에서 상하 반전을 안 해도 된다.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    wil::unique_hbitmap dib{
        CreateDIBSection(screenDc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (!dib || bits == nullptr) {
        SC_LOG(L"[캡처] CreateDIBSection 실패 err=%lu (%dx%d)", GetLastError(), width, height);
        return false;
    }

    // 이 DC는 여기서만 쓰고 버린다. 프레임을 다른 스레드로 넘기기 때문에
    // 스레드 친화성이 있는 DC를 들고 다니지 않는다.
    {
        wil::unique_hdc blitDc{CreateCompatibleDC(screenDc.get())};
        if (!blitDc) {
            SC_LOG(L"[캡처] CreateCompatibleDC 실패 err=%lu", GetLastError());
            return false;
        }
        auto scope = wil::SelectObject(blitDc.get(), dib.get());

        // CAPTUREBLT를 넣어야 레이어드 창(툴팁, 반투명 오버레이)이 함께 찍힌다.
        // 대신 8~10ms를 더 쓴다(2026-08-20 실측).
        if (!BitBlt(blitDc.get(), 0, 0, width, height, screenDc.get(), bounds.left, bounds.top,
                    SRCCOPY | CAPTUREBLT)) {
            SC_LOG(L"[캡처] BitBlt 실패 err=%lu", GetLastError());
            return false;
        }
        GdiFlush();
    }

    bitmap_ = std::move(dib);
    pixels_ = static_cast<uint32_t*>(bits);
    bounds_ = bounds;
    grabbedAt_ = GetTickCount64();

    SC_LOG(L"[캡처] 화면 읽기 %dx%d (%.1f MB) %.2f ms", width, height,
           static_cast<double>(width) * height * 4.0 / (1024.0 * 1024.0), watch.ElapsedMs());
    return true;
}

bool FrozenFrame::AttachDc() {
    if (!bitmap_) {
        return false;
    }
    if (memDc_) {
        return true;
    }

    wil::unique_hdc_window screenDc{wil::window_dc{GetDC(nullptr), nullptr}};
    wil::unique_hdc memDc{CreateCompatibleDC(screenDc.get())};
    if (!memDc) {
        SC_LOG(L"[캡처] AttachDc: CreateCompatibleDC 실패 err=%lu", GetLastError());
        return false;
    }
    selection_ = wil::SelectObject(memDc.get(), bitmap_.get());
    memDc_ = std::move(memDc);
    return true;
}

Bitmap32 FrozenFrame::Crop(const RECT& rect) const {
    Bitmap32 out;
    if (!HasPixels()) {
        return out;
    }

    // 프레임 안으로 자른다. 드래그가 화면 밖으로 나가도 안전하게 한다.
    RECT clipped{};
    if (!IntersectRect(&clipped, &rect, &bounds_)) {
        return out;
    }

    const int width = static_cast<int>(clipped.right - clipped.left);
    const int height = static_cast<int>(clipped.bottom - clipped.top);
    if (width <= 0 || height <= 0) {
        return out;
    }

    const int frameWidth = static_cast<int>(bounds_.right - bounds_.left);
    const int offsetX = static_cast<int>(clipped.left - bounds_.left);
    const int offsetY = static_cast<int>(clipped.top - bounds_.top);

    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y) {
        const uint32_t* src = pixels_ + static_cast<size_t>(offsetY + y) * frameWidth + offsetX;
        uint32_t* dst = out.Row(y);
        for (int x = 0; x < width; ++x) {
            // BitBlt로 뜬 화면은 알파가 쓰레기값이다. 불투명으로 채운다.
            dst[x] = src[x] | 0xFF000000u;
        }
    }
    return out;
}

}  // namespace sc
