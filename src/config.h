#pragma once

// Fixed thresholds.
//
// Anything the user can change lives in settings.h instead. What is left here
// is deliberately not a setting: these are tuned once and adding a knob for
// them would cost more in choices to make than it returns.

namespace sc::config {

// Drags shorter than this are discarded as accidental triggers, and a release
// inside the range is what turns a click into a whole-window capture.
constexpr int kMinDragPixels = 20;

// What happens after a capture. Both by default.
constexpr bool kCopyToClipboard = true;
constexpr bool kSaveToFile = true;

}  // namespace sc::config
