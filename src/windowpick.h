#pragma once

#include <windows.h>

namespace sc {

struct Bitmap32;

// Picking a whole window during a snapped drag.
//
// Two rules, tried in order:
//
//   1. The window under the cursor. Aiming anywhere inside a window is easy,
//      so this is what fires almost every time.
//   2. A window with a corner in the grid cell the drag started in. This only
//      gets a turn when the cursor is over the desktop or over something that
//      cannot be captured, and it exists for the case rule 1 cannot serve: a
//      target buried under other windows with only a corner still exposed.
//
// Enumerating and filtering top-level windows costs well under a millisecond,
// so this runs once per drag with no caching.

struct WindowPick {
    HWND hwnd = nullptr;
    RECT frame{};          // DWMWA_EXTENDED_FRAME_BOUNDS, virtual desktop pixels
    int cornerRadius = 0;  // 0 when the window has square corners
};

// The top-level window under pt, if it is something worth capturing.
bool PickWindowAt(POINT pt, WindowPick* out);

// The topmost window with a corner inside cell. Windows belonging to this
// process are skipped, as are windows whose matching corner is covered by
// something else, so a target hidden behind another window is never picked.
bool PickWindowByCorner(const RECT& cell, WindowPick* out);

// Clears the alpha channel in the four corners so a Windows 11 window does not
// carry the background showing through its rounded corners. No-op when radius
// is zero or the bitmap is too small.
void CarveRoundedCorners(Bitmap32& bitmap, int radius);

}  // namespace sc
