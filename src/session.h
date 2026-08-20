#pragma once

#include <windows.h>

#include <memory>

#include "capture.h"
#include "overlay.h"
#include "windowpick.h"

namespace sc {

// The lifetime of a single capture, driven by the messages the hook posts.
//
//   modifier down -> Prewarm : a worker thread grabs the screen in advance
//   button down   -> Begin   : use the pre-grabbed frame and show the overlay
//   move          -> Update  : update the selection rectangle only
//   button up     -> Finish  : crop, then copy to clipboard and save
//   right-click / Escape -> Cancel
//
// Reading the screen costs 55-70 ms on a single 4K monitor. Doing that on the
// UI thread would stall mouse input, because low-level hook callbacks are
// delivered to the thread that installed the hook. The read therefore happens
// only on a worker thread.
class CaptureSession {
public:
    void Init(HWND host);
    void Shutdown();

    bool Active() const { return active_; }

    void Prewarm();
    void DropPrewarm();

    void Begin(HWND owner);
    void Update();
    void Finish(HWND owner);
    void Cancel(const wchar_t* reason);

    // Re-reads the snap modifier and repaints if it changed. Called from the
    // drag timer so that pressing or releasing Shift takes effect even when the
    // mouse is standing still.
    void RefreshSnapState();

private:
    RECT CurrentSelection() const;
    bool WindowLatched() const;
    void Teardown();

    std::unique_ptr<FrozenFrame> frame_;
    Overlay overlay_;
    HWND host_ = nullptr;
    HWND owner_ = nullptr;
    bool active_ = false;

    // Snapping. The grid is anchored to the top-left of the monitor the drag
    // started on, so a selection is always a whole number of cells even when
    // monitors sit at odd offsets from the virtual desktop origin.
    bool snapEnabled_ = false;
    POINT gridOrigin_{};

    // Whole-window pick. Looked up once when the drag starts, from the grid
    // cell the drag began in. It stays selected while the cursor remains in
    // that cell, and leaving the cell hands control back to a normal drag, so
    // the gesture never traps the user in a selection they did not want.
    RECT startCell_{};
    WindowPick pick_{};
    bool hasPick_ = false;
};

// Timer used to watch for Escape during a drag. No keyboard hook is installed,
// so the message loop polls GetAsyncKeyState instead.
constexpr UINT_PTR kEscapeTimerId = 1;
constexpr UINT kEscapeTimerMs = 25;

}  // namespace sc
