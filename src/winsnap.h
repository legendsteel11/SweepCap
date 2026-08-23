#pragma once

#include <windows.h>

namespace sc::winsnap {

// Snapping other applications' windows to the grid.
//
// No mouse hook is involved, and nothing runs while a window is being dragged.
// A window event hook reports when a move or resize has finished; if the
// modifier is held at that moment, SetWindowPos is called once. The application
// being resized behaves exactly as it always does and we only intervene after
// it is done.
//
// The failure mode is therefore "not snapped" rather than "window broken". If
// SetWindowPos is ignored, the size the user dragged out simply stays. This is
// what separates it from the tools that snap every window automatically and
// break full-screen-windowed games doing it.
//
// The modifier cannot be held from the start of the drag: the capture hook
// swallows a button-down while it is down, so that would begin a capture
// instead. It is pressed part way through and held at release, the same way the
// snap key works during a capture drag.

// Whether a window is being moved or resized right now.
//
// The capture prewarm asks for this. While a window drag is running the mouse
// button is already held by that drag, so no capture can begin, and refreshing
// a full-screen grab every second and a half for the whole drag is pure waste:
// it costs 65 ms of a core and 32 MB of traffic each time on a 4K screen.
bool WindowDragActive();

bool Install();
void Remove();

}  // namespace sc::winsnap
