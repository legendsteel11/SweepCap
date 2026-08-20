#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wil/resource.h>

namespace sc {

// Owns a single tray icon.
//
// Shell_NotifyIcon loses the icon when Explorer restarts. The owner window has
// to receive the "TaskbarCreated" broadcast and call Restore().
class Tray {
public:
    Tray() = default;
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // owner must be a top-level window. Message-only windows (HWND_MESSAGE) do
    // not receive broadcasts such as TaskbarCreated.
    bool Add(HWND owner, UINT callbackMessage, UINT iconId);
    bool Restore();
    void Remove();

    UINT IconId() const { return iconId_; }

private:
    bool Register();

    HWND owner_ = nullptr;
    UINT callbackMessage_ = 0;
    UINT iconId_ = 0;
    wil::unique_hicon icon_;
    bool added_ = false;
};

}  // namespace sc
