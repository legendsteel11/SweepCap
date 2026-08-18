#include "tray.h"

#include <commctrl.h>

#include "../res/resource.h"
#include "app_name.h"
#include "log.h"

namespace sc {
namespace {

// 트레이용 작은 아이콘. LoadIconMetric은 DPI를 감안해 알맞은 크기를 고른다.
// (comctl32 v6가 필요하고 매니페스트에 선언해 뒀다.)
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

    // 버전 4를 쓰면 마우스 좌표가 wParam에 화면 좌표로 들어오고
    // 우클릭이 WM_CONTEXTMENU로 정규화된다.
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        SC_LOG(L"Shell_NotifyIcon(NIM_SETVERSION) 실패 err=%lu", GetLastError());
    }

    added_ = true;
    return true;
}

bool Tray::Restore() {
    // 탐색기가 재시작하면 이전 등록은 이미 사라졌다. 지우려 하지 않고 다시 넣는다.
    added_ = false;
    const bool ok = Register();
    SC_LOG(L"TaskbarCreated: 트레이 아이콘 재등록 %s", ok ? L"성공" : L"실패");
    return ok;
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
