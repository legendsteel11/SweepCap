#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace sc::settings {

// Everything the tray menu can change, held in memory and written straight
// back to an INI file next to the debug log.
//
// All of it is read and written on the UI thread. The low-level mouse hook is
// installed on that same thread, so its callbacks arrive there too, and the
// menu is handled in the same message loop. Nothing here is synchronised and
// nothing needs to be.
//
// Values that reach here from the INI are always matched against the choices
// below and fall back to the default when they do not match, so a hand-edited
// or corrupted file cannot produce a gesture the application does not offer.

// Modifier bits. Deliberately not the MOD_* values, which belong to
// RegisterHotKey and are not used for the drag gesture: modifier state is read
// with GetAsyncKeyState, because no keyboard hook is installed anywhere.
enum Modifier : int {
    kCtrl = 1 << 0,
    kAlt = 1 << 1,
    kShift = 1 << 2,
    kWin = 1 << 3,
};

// A gesture is a modifier set plus the one extra key that turns on grid
// snapping. The two are chosen as a pair so the snap key can never be part of
// the modifier itself, which would make the plain and the snapped gesture
// indistinguishable.
struct Gesture {
    int modifiers;
    int snap;      // exactly one bit, never overlapping modifiers
    UINT labelId;  // STRINGTABLE id
};

// Reads the INI. Call once, before the hook is installed.
void Load();

// The gestures the tray menu offers, in menu order. Index 0 is the default.
const Gesture* Gestures(size_t* count);
int GestureIndex();
void SetGestureIndex(int index);

// Whether the gesture's modifiers are held right now, and whether the snap key
// is on top of them. Called from the hook callback, so both do nothing beyond
// reading key state.
bool ModifiersHeld();
bool SnapModifierHeld();

// Grid for snapped selections.
//
// A value is either a fixed pixel pitch or a number of divisions of the
// monitor, and the value itself says which. Keeping both kinds in one list
// means offering the choice costs no extra setting: there is no mode to pick
// before picking a value.
//
// The two are not interchangeable. A pixel pitch keeps a cell the same size on
// every monitor, which is what aligning to a button edge needs. Divisions
// always land on the screen edges, which a pitch only manages by luck: a 32 px
// grid on a 1366 wide screen leaves a half cell at the right that can never be
// snapped to.
struct GridChoice {
    int px;    // 0 when this is a division choice
    int cols;  // 0 when this is a pixel choice
    int rows;

    bool ByDivision() const { return px <= 0; }
};

const GridChoice* GridChoices(size_t* count);
int GridIndex();
void SetGridIndex(int index);
const GridChoice& Grid();

// Grid that window sizes snap to. Same kinds of value as the capture grid and
// a separate setting, because the two measure different things.
//
// A window size is a fraction of the screen, so half of it has to land on a
// grid line, which only division guarantees. A capture is aligned to pixels of
// user interface, so a cell has to stay the same size when the monitor
// changes, which only a fixed pitch guarantees. Hence the same list of kinds
// and different defaults.
//
// Measured against the work area, not the whole monitor: a window must not go
// under the taskbar, while a capture may well want it in shot.
const GridChoice* WindowGridChoices(size_t* count);
int WindowGridIndex();
void SetWindowGridIndex(int index);
const GridChoice& WindowGrid();

// How much brightness the area outside the selection keeps, in eighths.
//
// Eight means no dimming at all, and then no darkened copy is built: the
// overlay already falls back to the original, so that also saves the copy's
// memory and the time spent making it.
//
// This is the one value read off the UI thread. The darkened copy is built by
// the worker that grabs the screen, so the value is atomic; everything else
// here is touched only by the message loop.
const int* DimChoices(size_t* count);
int DimIndex();
void SetDimIndex(int index);
int DimKeepEighths();

// The folder captures go under, before the per-date subfolder. Never empty:
// when nothing is configured this resolves to Pictures\SweepCap.
const std::wstring& CaptureRoot();
bool CaptureRootIsDefault();

// The folder used when nothing is configured: Pictures\SweepCap. Also the
// place a capture is diverted to when the configured folder stops working.
// Empty only when the Pictures folder itself cannot be resolved.
std::wstring DefaultCaptureRoot();

// Passing nullptr or an empty string restores the default.
void SetCaptureRoot(const wchar_t* path);

// Whether captures are filed into a per-date subfolder under the root. Off
// keeps everything directly in the root, for a folder that is bookmarked and
// browsed in one place; the file names carry the date either way.
bool DateFolders();
void SetDateFolders(bool on);

// Backed by the HKCU Run key, so no elevation is involved.
//
// Reading it tells you whether the value exists, not whether Windows will act
// on it: Task Manager can disable a startup entry without removing it, and
// that state is not visible here.
bool RunAtStartup();
bool SetRunAtStartup(bool on);

}  // namespace sc::settings
