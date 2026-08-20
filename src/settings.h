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

// Grid pitch for snapped selections.
const int* GridChoices(size_t* count);
int GridSizePx();
void SetGridSizePx(int px);

// The folder captures go under, before the per-date subfolder. Never empty:
// when nothing is configured this resolves to Pictures\SweepCap.
const std::wstring& CaptureRoot();
bool CaptureRootIsDefault();

// Passing nullptr or an empty string restores the default.
void SetCaptureRoot(const wchar_t* path);

// Backed by the HKCU Run key, so no elevation is involved.
//
// Reading it tells you whether the value exists, not whether Windows will act
// on it: Task Manager can disable a startup entry without removing it, and
// that state is not visible here.
bool RunAtStartup();
bool SetRunAtStartup(bool on);

}  // namespace sc::settings
