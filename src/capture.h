#pragma once

#include <windows.h>
#include <wil/resource.h>

#include <cstdint>
#include <vector>

namespace sc {

// 32bpp BGRA, top-down, 스트라이드 = width * 4.
// 잘라낸 결과이자 클립보드와 PNG 인코딩의 입력이다.
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

// 프리즈 프레임.
//
// 가상 데스크탑 전체를 한 장 떠서 들고 있는다. 잘라내기는 화면이 아니라 이
// 비트맵에서 하므로 선택 UI가 결과물에 찍히는 문제가 원천적으로 없다.
// 오버레이도 이 비트맵을 그린다.
//
// 화면에서 읽어 내리는 데 4K 한 대 기준 55~70ms가 든다(2026-08-20 실측).
// 그래서 두 단계로 나눠 둔다.
//
//   GrabPixels() : 어느 스레드에서 불러도 된다. DC를 남기지 않고 HBITMAP만 만든다.
//                  느린 쪽이 전부 여기 있으므로 워커 스레드에서 미리 부른다.
//   AttachDc()   : 그릴 스레드에서 부른다. 메모리 DC를 붙인다. 싸다.
//
// DC는 스레드 친화성이 있으므로 만든 스레드에서 쓴다. HBITMAP은 프로세스 공용이라
// 스레드를 넘겨도 된다.
class FrozenFrame {
public:
    FrozenFrame() = default;
    ~FrozenFrame() { Reset(); }

    FrozenFrame(const FrozenFrame&) = delete;
    FrozenFrame& operator=(const FrozenFrame&) = delete;

    bool GrabPixels();
    bool AttachDc();

    // GrabPixels + AttachDc. 같은 스레드에서 끝낼 때 쓴다.
    bool Grab() { return GrabPixels() && AttachDc(); }

    void Reset();

    bool HasPixels() const { return bitmap_ != nullptr; }
    bool Valid() const { return bitmap_ != nullptr && memDc_ != nullptr; }
    const RECT& Bounds() const { return bounds_; }
    HDC Dc() const { return memDc_.get(); }

    // 어둡게 만든 사본. 선택 영역 밖에 이걸 그대로 깐다.
    //
    // 예전에는 밝은 원본을 깔고 그 위를 AlphaBlend로 덮었는데, 픽셀마다 두 번
    // 쓰는 셈이라 그 사이에 모니터가 갱신되면 밝은 상태가 보였다. 드래그 중
    // 번쩍이던 원인이다. 오프스크린에 합성해서 막을 수도 있지만 4K 전체를
    // 합성하는 데 34ms가 들어서(2026-08-20 실측) 그 길로는 안 갔다.
    // 사본을 미리 만들어 두면 픽셀마다 한 번만 쓴다.
    HDC DimDc() const { return dimDc_.get(); }

    // GrabPixels가 끝난 시점의 GetTickCount64. 프레임이 얼마나 묵었는지 본다.
    ULONGLONG GrabbedAt() const { return grabbedAt_; }

    // Bounds() 좌표계의 사각형을 잘라낸다. 알파는 255로 채운다.
    // (BitBlt로 뜬 화면은 알파 채널이 쓰레기값이다.)
    Bitmap32 Crop(const RECT& rect) const;

private:
    // 소멸 순서가 중요하다. 선택 복원이 먼저, 그다음 비트맵, 그다음 DC다.
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
