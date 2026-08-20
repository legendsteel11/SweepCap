#pragma once

#include <windows.h>

// Default gestures and thresholds.
//
// These move into the settings window in a later stage. Until then they stay
// collected here rather than scattered through the code.

namespace sc::config {

// The modifier is Ctrl+Alt: the one shell drag modifier that is unclaimed and
// does not collide with input language switching.
//
// No keyboard hook is installed anywhere in this application; that keeps the
// antivirus heuristic profile low. Modifier state is read with GetAsyncKeyState.
inline bool ModifiersHeld() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

// Drags shorter than this are discarded as accidental triggers.
// A later stage turns this range into window-fit capture.
constexpr int kMinDragPixels = 20;

// What happens after a capture. Both by default.
constexpr bool kCopyToClipboard = true;
constexpr bool kSaveToFile = true;

}  // namespace sc::config
