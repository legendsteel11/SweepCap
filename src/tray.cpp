#include "tray.h"

#include <commctrl.h>

#include "../res/resource.h"
#include "app_name.h"
#include "log.h"

namespace sc {
namespace {

// Small icon for the tray. LoadIconMetric picks a size appropriate for the
// current DPI. It needs comctl32 v6, which the manifest declares.
wil::unique_hicon LoadTrayIcon() {
    HICON raw = nullptr;
    const HRESULT hr = LoadIconMetric(GetModuleHandleW(nullptr),
                                      MAKEINTRESOURCEW(IDI_APPICON), LIM_SMALL, &raw);
    if (FAILED(hr)) {
        SC_LOG(L"LoadIconMetric 실패 hr=0x%08lX", static_cast<unsigned long>(hr));
        return {};
    }
    return wil::unique_hicon{raw};
}

void LoadTip(wchar_t* buffer, size_t count) {
    if (LoadStringW(GetModuleHandleW(nullptr), IDS_TRAY_TIP, buffer,
                    static_cast<int>(count)) == 0) {
        wcscpy_s(buffer, count, SWEEPCAP_NAME_W);
    }
}

}  // namespace

Tray::~Tray() { Remove(); }

bool Tray::Add(HWND owner, UINT callbackMessage, UINT iconId) {
    owner_ = owner;
    callbackMessage_ = callbackMessage;
    iconId_ = iconId;
    icon_ = LoadTrayIcon();
    return Register();
}

bool Tray::Register() {
    if (!owner_) {
        return false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = iconId_;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = callbackMessage_;
    nid.hIcon = icon_.get();
    LoadTip(nid.szTip, ARRAYSIZE(nid.szTip));

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        SC_LOG(L"Shell_NotifyIcon(NIM_ADD) 실패 err=%lu", GetLastError());
        added_ = false;
        return false;
    }

    // Version 4 delivers mouse position in wParam as screen coordinates and
    // normalises right-click to WM_CONTEXTMENU.
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        SC_LOG(L"Shell_NotifyIcon(NIM_SETVERSION) 실패 err=%lu", GetLastError());
    }

    added_ = true;
    return true;
}

bool Tray::Restore() {
    // After an Explorer restart the previous registration is already gone, so
    // add a fresh one instead of trying to delete first.
    added_ = false;
    const bool ok = Register();
    SC_LOG(L"TaskbarCreated: 트레이 아이콘 재등록 %s", ok ? L"성공" : L"실패");
    return ok;
}

bool Tray::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!added_ || title == nullptr || text == nullptr) {
        return false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = iconId_;
    nid.uFlags = NIF_INFO;
    // NIIF_WARNING draws the system warning glyph. NIIF_NOSOUND is deliberately
    // not set: this only appears when a capture was lost, which is worth a
    // sound.
    nid.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    wcscpy_s(nid.szInfo, ARRAYSIZE(nid.szInfo), text);

    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        SC_LOG(L"Shell_NotifyIcon(NIF_INFO) 실패 err=%lu", GetLastError());
        return false;
    }
    return true;
}

void Tray::Remove() {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = iconId_;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

}  // namespace sc
