#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct Bitmap32;

// Delivery of a finished capture. Encoding runs once and both the clipboard
// and the file share the result.

// Encodes to PNG. Uses WIC, so the calling thread must have CoInitialize'd.
// Returns an empty vector on failure.
std::vector<uint8_t> EncodePng(const Bitmap32& bitmap);

// Places both CF_DIBV5 and the registered "PNG" format on the clipboard.
//
// CF_DIB alone loses alpha, and CF_DIBV5 alone gives inconsistent results
// depending on the receiving application. The shell synthesises CF_DIB and
// CF_BITMAP from CF_DIBV5.
bool CopyToClipboard(HWND owner, const Bitmap32& bitmap, const std::vector<uint8_t>& png);

// The diverted case still means the file exists, but it is not where the user
// believes captures go, so the caller has to report it. A file that quietly
// lands somewhere else is as good as lost.
enum class SaveResult {
    kFailed,
    kSaved,
    kSavedToDefault,  // the configured folder was unusable; the default took the file
};

// Saves under Pictures\SweepCap\<date>\ using the naming policy from CLAUDE.md:
// Chrome_2026-08-18_17-23-33.png
//
// appName is the app the capture came from and may be empty, in which case a
// fixed stand-in takes its place.
//
// When the configured folder cannot take the file (drive gone, permission
// denied, bad path), the default folder is tried instead. The setting is left
// alone, so an unplugged drive gets its captures back the moment it returns.
//
// Never overwrites. Colliding names get -2, -3 and so on.
// On success outPath receives the actual path written.
SaveResult SavePng(const std::vector<uint8_t>& png, const std::wstring& appName,
                   std::wstring& outPath);

}  // namespace sc
