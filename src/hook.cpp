#include "hook.h"

#include <wil/resource.h>

#include <atomic>
#include <cstdint>

#include "log.h"
#include "settings.h"

namespace sc::hook {
namespace {

// Coordinates are packed into a single 64-bit value. Kept as two separate
// atomics they could be read from different frames mid-movement; the result
// would only be a rectangle one frame out of date, but reading both at once
// costs nothing.
constexpr uint64_t Pack(LONG x, LONG y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) |
           static_cast<uint32_t>(x);
}
constexpr POINT Unpack(uint64_t v) {
    return POINT{static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFFu)),
                 static_cast<LONG>(static_cast<int32_t>(v >> 32))};
}

wil::unique_hhook g_hook;
HWND g_target = nullptr;  // set by Install; the callback only reads it

std::atomic<bool> g_dragging{false};
std::atomic<uint64_t> g_anchor{0};
std::atomic<uint64_t> g_current{0};

// Coalescing for WM_SC_DRAG_UPDATE: never post a second one while the first is
// still outstanding.
std::atomic<bool> g_updatePosted{false};

// A right-click cancel swallows the button-down, so the matching button-up has
// to be swallowed too. Otherwise the window underneath sees an unpaired up and
// opens a context menu.
std::atomic<bool> g_swallowRightUp{false};

// Movement notification throttle. An 8000 Hz mouse would otherwise flood the
// message queue. GetTickCount64 reads a shared page, so it is effectively free.
constexpr ULONGLONG kActivityNotifyMs = 100;

// Touched only by the hook callback, which runs on the thread that installed
// the hook, i.e. the same thread as the message loop.
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

    // RDP input can carry LLMHF_INJECTED. Filtering on that flag would turn
    // into a bug where the gesture works everywhere except over RDP, so the
    // flag is deliberately ignored.

    switch (wparam) {
        case WM_MOUSEMOVE:
            if (!dragging) {
                NotifyMouseActive();
                return CallNextHookEx(nullptr, code, wparam, lparam);  // return immediately
            }
            g_current.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            if (!g_updatePosted.exchange(true, std::memory_order_acq_rel)) {
                PostMessageW(g_target, WM_SC_DRAG_UPDATE, 0, 0);
            }
            // Movement is not swallowed. The overlay covers the screen so
            // nothing underneath sees it anyway, and letting it through is what
            // gives the overlay a WM_SETCURSOR to show the crosshair with.
            break;

        case WM_LBUTTONDOWN:
            if (dragging) {
                return 1;
            }
            if (!settings::ModifiersHeld()) {
                break;
            }
            g_anchor.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            g_current.store(Pack(info->pt.x, info->pt.y), std::memory_order_release);
            g_updatePosted.store(false, std::memory_order_release);
            g_dragging.store(true, std::memory_order_release);
            PostMessageW(g_target, WM_SC_DRAG_BEGIN, 0, 0);
            // Swallow this click. Letting it through would let the window
            // underneath start its own drag, such as a text selection.
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
