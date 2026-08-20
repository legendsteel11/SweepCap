#pragma once

#include <windows.h>

#include <memory>

#include "capture.h"
#include "overlay.h"

namespace sc {

// 캡처 한 번의 수명. 훅이 던진 메시지를 받아 순서대로 진행한다.
//
//   수식키 누름 -> Prewarm : 워커 스레드에서 화면을 미리 떠 둔다
//   버튼 다운   -> Begin   : 떠 둔 것이 있으면 그대로 쓰고 오버레이를 띄운다
//   이동        -> Update  : 선택 사각형만 갱신한다
//   버튼 업     -> Finish  : 잘라내고 클립보드와 파일로 내보낸다
//   우클릭/ESC  -> Cancel
//
// 화면 읽기가 4K 한 대에 55~70ms다(2026-08-20 실측). 이걸 UI 스레드에서 하면
// 저수준 훅 콜백이 같은 스레드로 오기 때문에 그동안 마우스 입력이 밀린다.
// 그래서 읽기는 워커 스레드에서만 한다.
class CaptureSession {
public:
    void Init(HWND host);
    void Shutdown();

    bool Active() const { return active_; }

    void Prewarm();
    void DropPrewarm();

    void Begin(HWND owner);
    void Update();
    void Finish(HWND owner);
    void Cancel(const wchar_t* reason);

private:
    RECT CurrentSelection() const;
    void Teardown();

    std::unique_ptr<FrozenFrame> frame_;
    Overlay overlay_;
    HWND host_ = nullptr;
    HWND owner_ = nullptr;
    bool active_ = false;
};

// 드래그 중 ESC를 보기 위한 타이머. 키보드 훅을 쓰지 않으므로
// 메시지 루프에서 GetAsyncKeyState로 확인한다.
constexpr UINT_PTR kEscapeTimerId = 1;
constexpr UINT kEscapeTimerMs = 25;

}  // namespace sc
