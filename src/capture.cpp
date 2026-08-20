#include "capture.h"

#include <algorithm>

#include "coords.h"
#include "log.h"

namespace sc {
namespace {

// Elapsed-time helper for instrumentation.
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
    dimSelection_.reset();
    dimBitmap_.reset();
    dimDc_.reset();
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

    // Top-down DIB. A negative biHeight puts the topmost screen row first, so
    // cropping and encoding never have to flip anything.
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

    // This DC is used here and discarded. The frame is handed to another
    // thread, so it must not carry a DC, which has thread affinity.
    {
        wil::unique_hdc blitDc{CreateCompatibleDC(screenDc.get())};
        if (!blitDc) {
            SC_LOG(L"[캡처] CreateCompatibleDC 실패 err=%lu", GetLastError());
            return false;
        }
        auto scope = wil::SelectObject(blitDc.get(), dib.get());

        // CAPTUREBLT is what makes layered windows - tooltips, translucent
        // overlays - appear in the capture. It costs 8-10 ms extra.
        if (!BitBlt(blitDc.get(), 0, 0, width, height, screenDc.get(), bounds.left, bounds.top,
                    SRCCOPY | CAPTUREBLT)) {
            SC_LOG(L"[캡처] BitBlt 실패 err=%lu", GetLastError());
            return false;
        }
        GdiFlush();
    }

    const double blitMs = watch.ElapsedMs();

    // Build the darkened copy the overlay lays down outside the selection.
    //
    // Exactly half brightness, which takes one shift and one mask per pixel
    // with no multiply. Working 64 bits at a time halves the iteration count.
    void* dimBits = nullptr;
    wil::unique_hbitmap dimDib{
        CreateDIBSection(screenDc.get(), &info, DIB_RGB_COLORS, &dimBits, nullptr, 0)};
    if (dimDib && dimBits != nullptr) {
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        const auto* src = static_cast<const uint64_t*>(bits);
        auto* dst = static_cast<uint64_t*>(dimBits);
        const size_t pairs = pixelCount / 2;
        for (size_t i = 0; i < pairs; ++i) {
            dst[i] = ((src[i] >> 1) & 0x7F7F7F7F7F7F7F7Full) | 0xFF000000FF000000ull;
        }
        if ((pixelCount & 1u) != 0) {
            const auto* src32 = static_cast<const uint32_t*>(bits);
            auto* dst32 = static_cast<uint32_t*>(dimBits);
            dst32[pixelCount - 1] = ((src32[pixelCount - 1] >> 1) & 0x7F7F7F7Fu) | 0xFF000000u;
        }
        dimBitmap_ = std::move(dimDib);
    } else {
        SC_LOG(L"[캡처] 어둡게 만든 사본 생성 실패 err=%lu", GetLastError());
    }

    bitmap_ = std::move(dib);
    pixels_ = static_cast<uint32_t*>(bits);
    bounds_ = bounds;
    grabbedAt_ = GetTickCount64();

    SC_LOG(L"[캡처] 화면 읽기 %dx%d (%.1f MB) %.2f ms (BitBlt %.2f / 어둡게 %.2f)", width,
           height, static_cast<double>(width) * height * 4.0 / (1024.0 * 1024.0),
           watch.ElapsedMs(), blitMs, watch.ElapsedMs() - blitMs);
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

    if (dimBitmap_) {
        wil::unique_hdc dim{CreateCompatibleDC(screenDc.get())};
        if (dim) {
            dimSelection_ = wil::SelectObject(dim.get(), dimBitmap_.get());
            dimDc_ = std::move(dim);
        }
    }
    return true;
}

Bitmap32 FrozenFrame::Crop(const RECT& rect) const {
    Bitmap32 out;
    if (!HasPixels()) {
        return out;
    }

    // Clip to the frame so a drag that leaves the screen stays safe.
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
            // A screen BitBlt leaves alpha undefined. Force it opaque.
            dst[x] = src[x] | 0xFF000000u;
        }
    }
    return out;
}

}  // namespace sc
