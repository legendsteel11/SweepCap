#pragma once

#include <windows.h>

// CLAUDE.md "조작 기본값".
// 5단계에서 설정 창으로 뺀다. 그때까지 상수를 여기 한 곳에 모아 둔다.
// 값을 코드 여기저기에 흩뿌리지 않는 것이 목적이다.

namespace sc::config {

// 수식키는 Ctrl+Alt다. 셸 드래그 수식키 중 유일하게 비어 있고
// 입력 언어 전환에도 안 걸린다.
//
// 키보드 훅을 쓰지 않는다. 백신 오탐 프로파일을 낮추기 위해서다.
// 수식키 상태는 마우스 훅 안에서 GetAsyncKeyState로 읽는다.
inline bool ModifiersHeld() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

// 이보다 짧은 드래그는 취소한다. 오발동 방지.
// 3단계에서 이 구간이 "창 fit 캡처"가 된다.
constexpr int kMinDragPixels = 20;

// 캡처 후 처리. 기본은 둘 다.
constexpr bool kCopyToClipboard = true;
constexpr bool kSaveToFile = true;

}  // namespace sc::config
