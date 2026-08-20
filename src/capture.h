#pragma once

#include <windows.h>
#include <wil/resource.h>

#include <cstdint>
#include <vector>

namespace sc {

// 32bpp BGRA, top-down, stride = width * 4.
// The result of a crop and the input to clipboard and PNG encoding.
struct Bitmap32 {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;

    bool Valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
    uint32_t* Row(int y) { return pixels.data() + static_cast<size_t>(y) * width; }
    const uint32_t* Row(int y) const { return pixels.data() + static_cast<size_t>(y) * width; }
    size_t ByteSize() const { return pixels.size() * sizeof(uint32_t); }
};

// A frozen frame: one snapshot of the whole virtual desktop.
//
// Cropping reads from this bitmap rather than from the live screen, so the
// selection UI can never end up in the saved image. The overlay draws from it
// as well.
//
// Reading the screen costs 55-70 ms on a single 4K monitor, which dominates
// everything else, so the work is split in two:
//
//   GrabPixels() - the slow half. Safe on any thread; produces an HBITMAP and
//                  keeps no DC, so a worker thread can run it ahead of time.
//   AttachDc()   - cheap. Called on the thread that will draw.
//
// DCs have thread affinity and are used only on the thread that created them.
// HBITMAPs are process-wide and can cross threads.
class FrozenFrame {
public:
    FrozenFrame() = default;
    ~FrozenFrame() { Reset(); }

    FrozenFrame(const FrozenFrame&) = delete;
    FrozenFrame& operator=(const FrozenFrame&) = delete;

    bool GrabPixels();
    bool AttachDc();

    // GrabPixels + AttachDc, for callers that stay on one thread.
    bool Grab() { return GrabPixels() && AttachDc(); }

    void Reset();

    bool HasPixels() const { return bitmap_ != nullptr; }
    bool Valid() const { return bitmap_ != nullptr && memDc_ != nullptr; }
    const RECT& Bounds() const { return bounds_; }
    HDC Dc() const { return memDc_.get(); }

    // A darkened copy, laid down outside the selection.
    //
    // Compositing the dim pass at paint time would mean writing each pixel
    // twice - once bright, once darkened - and a display refresh landing
    // between the two writes shows the bright state for a frame. Preparing the
    // copy up front keeps painting to exactly one write per pixel.
    HDC DimDc() const { return dimDc_.get(); }

    // GetTickCount64 at the moment GrabPixels finished, used to decide whether
    // a pre-grabbed frame is still fresh enough to use.
    ULONGLONG GrabbedAt() const { return grabbedAt_; }

    // Crops a rectangle given in Bounds() coordinates. Alpha is forced to 255
    // because a screen BitBlt leaves the alpha channel undefined.
    Bitmap32 Crop(const RECT& rect) const;

private:
    // Destruction order matters: restore the selection, then the bitmap,
    // then the DC.
    wil::unique_hdc memDc_;
    wil::unique_hbitmap bitmap_;
    wil::unique_select_object selection_;

    wil::unique_hdc dimDc_;
    wil::unique_hbitmap dimBitmap_;
    wil::unique_select_object dimSelection_;

    uint32_t* pixels_ = nullptr;
    RECT bounds_{};
    ULONGLONG grabbedAt_ = 0;
};

}  // namespace sc
