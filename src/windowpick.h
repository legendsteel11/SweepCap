#pragma once

#include <windows.h>

namespace sc {

struct Bitmap32;

// Picking a whole window by the grid cell a drag starts in.
//
// Starting the drag in a cell that holds one of a window's corners selects that
// entire window. It reaches windows that a cursor-position test cannot: another
// window may cover most of the target, and as long as one corner is exposed the
// target is still reachable.
//
// Enumerating and filtering top-level windows costs well under a millisecond,
// so this runs once per drag with no caching.

struct WindowPick {
    HWND hwnd = nullptr;
    RECT frame{};          // DWMWA_EXTENDED_FRAME_BOUNDS, virtual desktop pixels
    int cornerRadius = 0;  // 0 when the window has square corners
};

// Finds the topmost window with a corner inside cell. Windows belonging to this
// process are skipped, as are windows whose matching corner is covered by
// something else, so a target hidden behind another window is never picked.
bool PickWindowByCorner(const RECT& cell, WindowPick* out);

// Clears the alpha channel in the four corners so a Windows 11 window does not
// carry the background showing through its rounded corners. No-op when radius
// is zero or the bitmap is too small.
void CarveRoundedCorners(Bitmap32& bitmap, int radius);

}  // namespace sc
