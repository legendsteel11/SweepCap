#include "hook.h"

#include <wil/resource.h>

#include <atomic>
#include <cstdint>

#include "config.h"
#include "log.h"

namespace sc::hook {
namespace {

// 좌표는 64비트 하나에 담는다. x와 y를 따로 두면 이동 중에 둘이 서로 다른
// 프레임의 값으로 읽힐 수 있다. 결과는 한 프레임 어긋난 사각형이라 치명적이지는
// 않지만, 한 번에 읽는 편이 값이 싸다.
constexpr uint64_t Pack(LONG x, LONG y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) |
           static_cast<uint32_t>(x);
}
constexpr POINT Unpack(uint64_t v) {
    return POINT{static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFFu)),
                 static_cast<LONG>(static_cast<int32_t>(v >> 32))};
}

wil::unique_hhook g_hook;
HWND g_target = nullptr;  // Install에서만 쓴다. 콜백에서는 읽기만 한다.

std::atomic<bool> g_dragging{false};
std::atomic<uint64_t> g_anchor{0};
std::atomic<uint64_t> g_current{0};

// WM_SC_DRAG_UPDATE 병합용. 이미 던져 둔 메시지가 있으면 또 던지지 않는다.
std::atomic<bool> g_updatePosted{false};

// 우클릭 취소로 버튼 다운을 삼켰으면 짝이 되는 업도 삼켜야 한다.
// 안 그러면 밑의 앱이 짝 없는 업을 받아 컨텍스트 메뉴를 띄운다.
std::atomic<bool> g_swallowRightUp{false};

// 마우스가 살아 있다는 신호만 던진다. 판단은 메시지 루프 쪽에서 한다.
//
// 8000Hz 마우스를 감안해 100ms에 한 번만 던진다. GetTickCount64는 공유 페이지
// 읽기라 사실상 공짜다. 콜백 안에서 하는 일은 이 비교와 PostMessage 하나뿐이다.
constexpr ULONGLONG kActivityNotifyMs = 100;

// 훅 콜백에서만 만진다. 저수준 훅 콜백은 훅을 설치한 스레드로 오므로
// 메시지 루프와 같은 스레드다.
ULONGLONG g_lastActivityPost = 0;

void NotifyMouseActive() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastActivityPost < kActivityNotifyMs) {
        return;
    }
    g_lastActivityPost = now;
    PostMessageW(g_target, WM_SC_MOUSE_ACTIVE, 0, 0);
}

void EndDrag(UINT message) {
    g_dragging.store(false, std::memory_order_release);
    g_updatePosted.store(false, std::memory_order_release);
    PostMessageW(g_target, message, 0, 0);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    const bool dragging = g_dragging.load(std::memory_order_acquire);

    // RDP 입력에는 LLMHF_INJECTED가 붙을 수 있다. 이 플래그로 거르면
    // RDP에서만 제스처가 안 먹는 버그가 된다. 그래서 보지 않는다.

    switch (wparam) {
        case WM_MOUSEMOVE:
            if (!dragging) {
                NotifyMouseActive();
                return CallNextHookEx(nullptr, code, wparam, lparam);  // 즉시 반환
            }
            g_current.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            if (!g_updatePosted.exchange(true, std::memory_order_acq_rel)) {
                PostMessageW(g_target, WM_SC_DRAG_UPDATE, 0, 0);
            }
            // 이동은 삼키지 않는다. 오버레이가 화면을 덮고 있어서 밑의 앱에는
            // 어차피 안 간다. 대신 오버레이가 WM_SETCURSOR를 받아 십자 커서를
            // 띄울 수 있다.
            break;

        case WM_LBUTTONDOWN:
            if (dragging) {
                return 1;
            }
            if (!config::ModifiersHeld()) {
                break;
            }
            g_anchor.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            g_current.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            g_updatePosted.store(false, std::memory_order_release);
            g_dragging.store(true, std::memory_order_release);
            PostMessageW(g_target, WM_SC_DRAG_BEGIN, 0, 0);
            // 이 클릭을 삼킨다. 안 삼키면 밑의 앱이 자기 드래그(텍스트 선택 등)를
            // 먼저 시작한다.
            return 1;

        case WM_LBUTTONUP:
            if (!dragging) {
                break;
            }
            g_current.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            EndDrag(WM_SC_DRAG_END);
            return 1;

        case WM_RBUTTONDOWN:
            if (!dragging) {
                break;
            }
            g_swallowRightUp.store(true, std::memory_order_release);
            EndDrag(WM_SC_DRAG_CANCEL);
            return 1;

        case WM_RBUTTONUP:
            if (g_swallowRightUp.exchange(false, std::memory_order_acq_rel)) {
                return 1;
            }
            break;

        default:
            break;
    }

    return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace

bool Install(HWND target) {
    if (g_hook) {
        return true;
    }
    g_target = target;
    g_hook.reset(SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0));
    if (!g_hook) {
        SC_LOG(L"[훅] SetWindowsHookEx(WH_MOUSE_LL) 실패 err=%lu", GetLastError());
        return false;
    }
    SC_LOG(L"[훅] 저수준 마우스 훅 설치됨");
    return true;
}

void Remove() {
    if (!g_hook) {
        return;
    }
    g_hook.reset();
    g_dragging.store(false, std::memory_order_release);
    SC_LOG(L"[훅] 해제됨");
}

bool Installed() { return static_cast<bool>(g_hook); }

POINT Anchor() { return Unpack(g_anchor.load(std::memory_order_acquire)); }
POINT Current() { return Unpack(g_current.load(std::memory_order_acquire)); }

void AcknowledgeUpdate() { g_updatePosted.store(false, std::memory_order_release); }

bool Dragging() { return g_dragging.load(std::memory_order_acquire); }

void CancelDrag() {
    if (!g_dragging.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    g_updatePosted.store(false, std::memory_order_release);
}

}  // namespace sc::hook
