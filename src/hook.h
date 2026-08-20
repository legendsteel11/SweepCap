#pragma once

#include <windows.h>

namespace sc::hook {

// 저수준 마우스 훅.
//
// CLAUDE.md "설계 규칙": 훅 콜백 안에서는 아무것도 하지 않는다.
// 할당, 로그, 잠금, 블로킹 전부 금지다. 8000Hz 마우스면 초당 8000번 불리고,
// Windows는 콜백이 제 시간에 반환하지 않으면 훅을 조용히 건너뛰거나 해제한다.
// 여기서는 원자 변수에 좌표만 적고 메시지 하나를 던진다.
//
// 드래그 중이 아닐 때의 WM_MOUSEMOVE는 즉시 반환한다.

// 훅이 앱 창으로 던지는 메시지.
constexpr UINT WM_SC_DRAG_BEGIN = WM_APP + 10;
constexpr UINT WM_SC_DRAG_UPDATE = WM_APP + 11;  // 병합된다. 최신 좌표만 의미가 있다.
constexpr UINT WM_SC_DRAG_END = WM_APP + 12;
constexpr UINT WM_SC_DRAG_CANCEL = WM_APP + 13;

// 마우스가 움직이고 있다는 신호. 100ms에 한 번만 던진다.
//
// 이걸 받은 쪽이 수식키 감시 타이머를 돌린다. 훅에서 직접 수식키를 보지 않는
// 이유는, 실제 조작 순서가 "커서를 옮기고 -> Ctrl+Alt를 누르고 -> 클릭"이라서
// 수식키를 누른 뒤에는 마우스 이동이 없기 때문이다. 이동에 얹어서만 보면
// 정작 필요한 순간을 놓친다.
//
// 타이머는 마우스가 조용해지면 스스로 멈춘다. 상주 중에 계속 깨어 있지 않는다.
constexpr UINT WM_SC_MOUSE_ACTIVE = WM_APP + 14;

bool Install(HWND target);
void Remove();
bool Installed();

// 가상 데스크탑 좌표(물리 픽셀). 음수일 수 있다.
POINT Anchor();
POINT Current();

// WM_SC_DRAG_UPDATE를 처리한 뒤 부른다. 다음 이동에서 다시 메시지를 받는다.
void AcknowledgeUpdate();

// 드래그 중인지. 훅이 입력을 삼키고 있는 구간이다.
bool Dragging();

// 바깥에서 드래그를 끊는다(ESC 등). 훅 상태만 되돌린다.
void CancelDrag();

}  // namespace sc::hook
