#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wil/resource.h>

namespace sc {

// 트레이 아이콘 하나를 관리한다.
//
// Shell_NotifyIcon은 탐색기가 재시작하면 아이콘을 잃어버린다. 소유 창이
// "TaskbarCreated" 브로드캐스트를 받아 Restore()를 불러 줘야 한다.
class Tray {
public:
    Tray() = default;
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // owner는 반드시 최상위 창이어야 한다. 메시지 전용 창(HWND_MESSAGE)은
    // TaskbarCreated 같은 브로드캐스트를 받지 못한다.
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
