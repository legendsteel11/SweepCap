#pragma once

#include <windows.h>

namespace sc::hook {

// Low-level mouse hook.
//
// The callback does nothing but record coordinates in atomics and post one
// message. No allocation, no locking, no logging: an 8000 Hz mouse calls it
// 8000 times a second, and Windows silently skips or unhooks a callback that
// does not return in time. WM_MOUSEMOVE returns immediately when no drag is in
// progress.

// Messages the hook posts to the application window.
constexpr UINT WM_SC_DRAG_BEGIN = WM_APP + 10;
constexpr UINT WM_SC_DRAG_UPDATE = WM_APP + 11;  // coalesced; only the latest position matters
constexpr UINT WM_SC_DRAG_END = WM_APP + 12;
constexpr UINT WM_SC_DRAG_CANCEL = WM_APP + 13;

// Signals that the mouse is moving. Posted at most every 100 ms.
//
// The receiver uses it to run a modifier-watch timer. The hook does not check
// the modifiers itself because the real gesture order is "move the cursor, then
// press Ctrl+Alt, then click" - there is no mouse movement after the modifier
// goes down, so watching only on movement misses the moment that matters.
//
// The timer stops itself once the mouse goes quiet, so nothing polls while the
// application sits idle in the tray.
constexpr UINT WM_SC_MOUSE_ACTIVE = WM_APP + 14;

bool Install(HWND target);
void Remove();
bool Installed();

// Virtual desktop coordinates in physical pixels. Can be negative.
POINT Anchor();
POINT Current();

// Call after handling WM_SC_DRAG_UPDATE to re-arm the next notification.
void AcknowledgeUpdate();

// True while a drag is in progress, which is also while the hook swallows input.
bool Dragging();

// Aborts a drag from outside the hook, for example on Escape.
// Only resets hook state.
void CancelDrag();

}  // namespace sc::hook
