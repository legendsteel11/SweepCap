#pragma once

#include <windows.h>

#include <memory>
#include <string>

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

    // Takes the overlay down once the after-the-fact border has been shown
    // long enough. Driven by kFlashTimerId.
    void EndFlash();

    // Runs on the drag timer. Picks up a change of the snap modifier and the
    // moment the whole-window highlight becomes due, both of which have to
    // happen even while the mouse is standing still.
    void Tick();

private:
    RECT RawOrSnapped() const;
    RECT CurrentSelection() const;  // what the overlay should show
    bool WindowLatched() const;     // what a release would capture
    bool WindowShown() const;       // whether the highlight is due yet
    void UpdatePickRelease();
    void Teardown();

    std::unique_ptr<FrozenFrame> frame_;
    Overlay overlay_;
    HWND host_ = nullptr;
    HWND owner_ = nullptr;
    bool active_ = false;

    // Snapping. The grid is anchored to the monitor the drag
    // started on, so a selection is always a whole number of cells even when
    // monitors sit at odd offsets from the virtual desktop origin.
    bool snapEnabled_ = false;
    RECT gridArea_{};

    // Whole-window pick, looked up once when the drag starts.
    //
    // The rule the user sees is simply "click for a window, drag for a
    // rectangle": the pick survives until the drag passes the minimum distance
    // and is then given up for good, so a real drag can never end up capturing
    // a whole window.
    //
    // The highlight is held back briefly. On button-down there is no way to
    // know yet whether this is a click or a drag, and painting the window
    // immediately would make every snapped drag start with a full-window flash.
    // A drag passes the threshold well before the delay expires, so the flash
    // never appears; standing still brings up the highlight to say what a
    // release would capture.
    RECT startCell_{};
    WindowPick pick_{};
    bool hasPick_ = false;
    bool pickReleased_ = false;
    const wchar_t* pickRule_ = nullptr;  // which rule matched, for the log

    // Names the saved file. Read when the drag starts rather than when it
    // ends, so a window that closes underneath the capture still gets named.
    std::wstring appName_;

    bool windowShownLast_ = false;
    bool flashing_ = false;
    ULONGLONG dragStartedAt_ = 0;
};

// Posted when a capture could not be delivered. wParam carries the failures as
// a combination of the bits below.
//
// The session raises it rather than showing anything itself: the tray icon
// belongs to the application, and routing through the message loop keeps this
// out of the path that has to stay fast.
constexpr UINT WM_SC_DELIVERY_FAILED = WM_APP + 20;

enum DeliveryFailure : WPARAM {
    kDeliveryClipboardFailed = 1 << 0,
    kDeliverySaveFailed = 1 << 1,
    kDeliveryCaptureFailed = 1 << 2,  // nothing was produced to deliver
    // The save worked, but into the default folder because the configured one
    // was unusable. Reported through the same channel: a file that quietly
    // lands somewhere else is as good as lost. Never set with SaveFailed.
    kDeliverySaveFellBack = 1 << 3,
};

// Timer used to watch for Escape during a drag. No keyboard hook is installed,
// so the message loop polls GetAsyncKeyState instead.
constexpr UINT_PTR kEscapeTimerId = 1;
constexpr UINT kEscapeTimerMs = 25;

// How long the whole-window highlight waits before appearing.
constexpr ULONGLONG kWindowHighlightDelayMs = 120;

// Capturing a whole window from a plain click is over before the highlight was
// ever due, so nothing appears on screen and there is no way to tell it
// happened. The border is therefore painted once the capture is finished and
// held briefly.
//
// This delays nothing. The image is cropped from the frozen frame, which does
// not care what is on screen; only the teardown waits.
constexpr UINT_PTR kFlashTimerId = 3;
constexpr UINT kFlashMs = 160;

}  // namespace sc
