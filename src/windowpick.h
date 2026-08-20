#pragma once

#include <windows.h>

#include <string>

namespace sc {

struct Bitmap32;

// Picking a whole window during a snapped drag.
//
// Two rules, tried in order:
//
//   1. The window under the cursor. Aiming anywhere inside a window is easy,
//      so this is what fires almost every time.
//   2. A window with a corner in the grid cell the drag started in. This only
//      gets a turn when the cursor is over the desktop, and it exists for the
//      case rule 1 cannot serve: a target buried under other windows with only
//      a corner still exposed.
//   3. The monitor under the cursor. Clicking bare desktop captures that whole
//      screen.
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

// The whole monitor under pt. This is the last resort, used when the cursor is
// over bare desktop and no exposed corner is in reach: clicking empty space
// captures that screen.
bool PickMonitorAt(POINT pt, WindowPick* out);

// The short name of the app a window belongs to, taken from its executable
// ("chrome.exe" -> "Chrome"). Saved file names lead with this. Returns an
// empty string when the owning process cannot be read, which is also what a
// monitor pick produces, since that one has no window at all.
std::wstring AppNameForWindow(HWND hwnd);

// Clears the alpha channel in the four corners so a Windows 11 window does not
// carry the background showing through its rounded corners. No-op when radius
// is zero or the bitmap is too small.
void CarveRoundedCorners(Bitmap32& bitmap, int radius);

}  // namespace sc
