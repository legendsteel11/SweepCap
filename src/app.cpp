#include "app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <string>

#include "../res/resource.h"
#include "app_name.h"
#include "build_info.h"
#include "config.h"
#include "coords.h"
#include "hook.h"
#include "log.h"
#include "session.h"
#include "settings.h"
#include "tray.h"
#include "winsnap.h"

namespace sc {
namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

UINT g_taskbarCreated = 0;  // RegisterWindowMessage("TaskbarCreated")
Tray g_tray;
CaptureSession g_session;

// Loads a user-visible string from the string table.
//
// The fallback only fires when the resource is missing, which means a broken
// build; it is English on purpose so it never has to be translated.
const wchar_t* LoadText(UINT id, const wchar_t* fallback, wchar_t* buffer, int count) {
    if (LoadStringW(GetModuleHandleW(nullptr), id, buffer, count) == 0) {
        wcscpy_s(buffer, static_cast<size_t>(count), fallback);
    }
    return buffer;
}

// Everything the user can change is a menu item.
//
// The settings are all single choices from a short list, which a radio
// submenu shows and changes in one click while the current value is visible
// without opening anything. A settings window would need two clicks more per
// change and a second place for the same values to drift out of sync.

// Keeps the tail of a long path, which is the part that says which folder it
// is, and marks the cut so a shortened path is never mistaken for a real one.
std::wstring ShortenPath(const std::wstring& path) {
    constexpr size_t kMaxChars = 44;
    if (path.size() <= kMaxChars) {
        return path;
    }
    return L"..." + path.substr(path.size() - (kMaxChars - 3));
}

// Each builder keeps its own string buffer rather than borrowing the caller's.
// Sharing one buffer with the caller is unsafe here: a builder and the
// LoadText that reads the submenu's own title are arguments of the same call,
// and the order those are evaluated in is not defined. Sharing the buffer put
// the last item's text in the submenu title.

wil::unique_hmenu BuildGestureMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    wchar_t label[256];
    size_t total = 0;
    const settings::Gesture* items = settings::Gestures(&total);
    for (size_t i = 0; i < total; ++i) {
        AppendMenuW(menu.get(), MF_STRING, IDM_GESTURE_FIRST + i,
                    LoadText(items[i].labelId, L"Modifier", label, ARRAYSIZE(label)));
    }
    const UINT current = IDM_GESTURE_FIRST + static_cast<UINT>(settings::GestureIndex());
    CheckMenuRadioItem(menu.get(), IDM_GESTURE_FIRST,
                       IDM_GESTURE_FIRST + static_cast<UINT>(total) - 1, current,
                       MF_BYCOMMAND);
    return menu;
}

// One grid value as text: "32 px" or "12 x 6 분할".
const wchar_t* GridLabel(const settings::GridChoice& grid, wchar_t* buffer, int count) {
    if (grid.ByDivision()) {
        wchar_t format[64];
        LoadText(IDS_GRID_DIVISIONS, L"%d x %d divisions", format, ARRAYSIZE(format));
        if (swprintf_s(buffer, static_cast<size_t>(count), format, grid.cols, grid.rows) < 0) {
            buffer[0] = L'\0';
        }
    } else if (swprintf_s(buffer, static_cast<size_t>(count), L"%d px", grid.px) < 0) {
        buffer[0] = L'\0';
    }
    return buffer;
}

// There are two grids, and which one a submenu belongs to is not obvious from
// its name alone when both are shut. Putting the current value in the title
// means the pair can be read without opening either, which is how the wrong
// one got changed.
const wchar_t* GridTitle(UINT titleId, const wchar_t* fallback,
                         const settings::GridChoice& grid, wchar_t* buffer, int count) {
    wchar_t name[64];
    wchar_t value[64];
    LoadText(titleId, fallback, name, ARRAYSIZE(name));
    GridLabel(grid, value, ARRAYSIZE(value));
    if (swprintf_s(buffer, static_cast<size_t>(count), L"%s: %s", name, value) < 0) {
        wcscpy_s(buffer, static_cast<size_t>(count), name);
    }
    return buffer;
}

// Both grid menus are built the same way; only the list and the command range
// differ. Divisions and pixel pitches share one radio list, so the value says
// which kind it is and there is no mode to choose first.
wil::unique_hmenu BuildGridMenuFrom(const settings::GridChoice* choices, size_t total,
                                    int selected, UINT firstId) {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu || choices == nullptr || total == 0) {
        return {};
    }
    bool separated = false;
    const bool divisionsFirst = choices[0].ByDivision();
    for (size_t i = 0; i < total; ++i) {
        // A separator marks where one kind ends and the other begins.
        if (choices[i].ByDivision() != divisionsFirst && !separated) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            separated = true;
        }
        wchar_t label[64];
        AppendMenuW(menu.get(), MF_STRING, firstId + i,
                    GridLabel(choices[i], label, ARRAYSIZE(label)));
    }
    CheckMenuRadioItem(menu.get(), firstId, firstId + static_cast<UINT>(total) - 1,
                       firstId + static_cast<UINT>(selected), MF_BYCOMMAND);
    return menu;
}

wil::unique_hmenu BuildGridMenu() {
    size_t total = 0;
    const settings::GridChoice* choices = settings::GridChoices(&total);
    return BuildGridMenuFrom(choices, total, settings::GridIndex(), IDM_GRID_FIRST);
}

wil::unique_hmenu BuildWindowGridMenu() {
    size_t total = 0;
    const settings::GridChoice* choices = settings::WindowGridChoices(&total);
    return BuildGridMenuFrom(choices, total, settings::WindowGridIndex(), IDM_WGRID_FIRST);
}

wil::unique_hmenu BuildDimMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    // In menu order, matching settings::DimChoices.
    constexpr UINT kLabels[] = {IDS_DIM_OFF, IDS_DIM_LIGHT, IDS_DIM_NORMAL, IDS_DIM_STRONG};
    wchar_t label[64];
    size_t total = 0;
    settings::DimChoices(&total);
    if (total > ARRAYSIZE(kLabels)) {
        total = ARRAYSIZE(kLabels);
    }
    for (size_t i = 0; i < total; ++i) {
        AppendMenuW(menu.get(), MF_STRING, IDM_DIM_FIRST + i,
                    LoadText(kLabels[i], L"Dim", label, ARRAYSIZE(label)));
    }
    const UINT current = IDM_DIM_FIRST + static_cast<UINT>(settings::DimIndex());
    CheckMenuRadioItem(menu.get(), IDM_DIM_FIRST,
                       IDM_DIM_FIRST + static_cast<UINT>(total) - 1, current, MF_BYCOMMAND);
    return menu;
}

wil::unique_hmenu BuildFolderMenu() {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return {};
    }
    wchar_t label[256];
    // The current folder heads the submenu as a greyed line. It is the one
    // setting whose value is a path, so a checkmark cannot express it.
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0,
                ShortenPath(settings::CaptureRoot()).c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, IDM_FOLDER_CHANGE,
                LoadText(IDS_MENU_FOLDER_CHANGE, L"Change...", label, ARRAYSIZE(label)));
    AppendMenuW(
        menu.get(), MF_STRING | (settings::CaptureRootIsDefault() ? MF_GRAYED : 0),
        IDM_FOLDER_DEFAULT,
        LoadText(IDS_MENU_FOLDER_DEFAULT, L"Restore default folder", label, ARRAYSIZE(label)));
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    // Checked means captures land directly in the root, for a folder that is
    // bookmarked and browsed in one place. The file names carry the date
    // either way.
    AppendMenuW(menu.get(),
                MF_STRING | (settings::DateFolders() ? MF_UNCHECKED : MF_CHECKED),
                IDM_FOLDER_NO_DATE,
                LoadText(IDS_MENU_FOLDER_NO_DATE, L"No date subfolders", label,
                         ARRAYSIZE(label)));
    return menu;
}

// Menu row height.
//
// A popup menu has no row-height setting, but the row grows to fit the item's
// bitmap. A transparent bitmap of the wanted height therefore opens up the
// rows while the system keeps drawing the text and the theme itself. Rows
// never shrink: a height below the natural one simply does nothing.
constexpr int kMenuRowDip = 22;

wil::unique_hbitmap CreateSpacerBitmap(int width, int height, void** outBits) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    wil::unique_hbitmap bitmap{
        CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (bitmap && bits != nullptr) {
        // Alpha zero everywhere, so only the height of the bitmap is visible.
        memset(bits, 0, static_cast<size_t>(width) * height * 4);
    }
    if (outBits != nullptr) {
        *outBits = bits;
    }
    return bitmap;
}

// The blank spacer for ordinary items.
wil::unique_hbitmap CreateBlankSpacer(int height) {
    if (height <= 0) {
        return {};
    }
    return CreateSpacerBitmap(1, height, nullptr);
}

// The spacer for checked items, with the menu mark drawn in.
//
// An item's bitmap takes over the themed check area, so a checked item that
// got the blank spacer would lose its radio dot or check mark; the first
// version did exactly that. DrawFrameControl draws the classic glyph in black
// on white, and turning brightness into coverage lands it antialiased on the
// otherwise transparent spacer.
wil::unique_hbitmap CreateMarkSpacer(int height, int markSide, UINT glyph) {
    if (height <= 0 || markSide <= 0) {
        return {};
    }
    void* bits = nullptr;
    wil::unique_hbitmap bitmap = CreateSpacerBitmap(markSide, height, &bits);
    if (!bitmap || bits == nullptr) {
        return {};
    }
    wil::unique_hdc dc{CreateCompatibleDC(nullptr)};
    if (!dc) {
        return {};
    }
    HGDIOBJ previous = SelectObject(dc.get(), bitmap.get());
    RECT whole{0, 0, markSide, height};
    FillRect(dc.get(), &whole, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    RECT mark{0, (height - markSide) / 2, markSide, (height - markSide) / 2 + markSide};
    DrawFrameControl(dc.get(), &mark, DFC_MENU, glyph);
    SelectObject(dc.get(), previous);
    GdiFlush();

    auto* pixel = static_cast<uint32_t*>(bits);
    const size_t total = static_cast<size_t>(markSide) * height;
    for (size_t i = 0; i < total; ++i) {
        // Premultiplied black at the glyph's darkness.
        const uint32_t alpha = 255u - (pixel[i] & 0xFFu);
        pixel[i] = alpha << 24;
    }
    return bitmap;
}

struct MenuSpacers {
    wil::unique_hbitmap blank;
    wil::unique_hbitmap radio;
    wil::unique_hbitmap check;
};

// Separators keep their slim height on purpose; giving them a spacer would
// blow them up to full rows.
void ApplyMenuSpacers(HMENU menu, const MenuSpacers& spacers) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) {
            continue;
        }
        if (item.hSubMenu != nullptr) {
            ApplyMenuSpacers(item.hSubMenu, spacers);
        }
        if ((item.fType & MFT_SEPARATOR) != 0) {
            continue;
        }
        HBITMAP use = spacers.blank.get();
        if ((item.fState & MFS_CHECKED) != 0) {
            use = (item.fType & MFT_RADIOCHECK) != 0 ? spacers.radio.get()
                                                     : spacers.check.get();
        }
        MENUITEMINFOW change{};
        change.cbSize = sizeof(change);
        change.fMask = MIIM_BITMAP;
        change.hbmpItem = use;
        SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &change);
    }
}

// MF_POPUP hands the submenu to the parent, which destroys it in turn, so the
// wrapper has to let go of it.
void AttachSubmenu(HMENU parent, wil::unique_hmenu submenu, const wchar_t* label) {
    if (!submenu) {
        return;
    }
    if (AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu.get()), label)) {
        submenu.release();
    }
}

void ShowTrayMenu(HWND hwnd, POINT screenPoint) {
    wil::unique_hmenu menu{CreatePopupMenu()};
    if (!menu) {
        return;
    }

    wchar_t text[256];

    AppendMenuW(menu.get(), MF_STRING, IDM_OPEN_FOLDER,
                LoadText(IDS_MENU_OPEN_FOLDER, L"Open capture folder", text, ARRAYSIZE(text)));
    SetMenuDefaultItem(menu.get(), IDM_OPEN_FOLDER, FALSE);
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);

    AttachSubmenu(menu.get(), BuildGestureMenu(),
                  LoadText(IDS_MENU_GESTURE, L"Change shortcut", text, ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildGridMenu(),
                  GridTitle(IDS_MENU_GRID, L"Capture grid", settings::Grid(), text,
                            ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildWindowGridMenu(),
                  GridTitle(IDS_MENU_WINDOW_GRID, L"Window grid", settings::WindowGrid(),
                            text, ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildDimMenu(),
                  LoadText(IDS_MENU_DIM, L"Dim outside", text, ARRAYSIZE(text)));
    AttachSubmenu(menu.get(), BuildFolderMenu(),
                  LoadText(IDS_MENU_FOLDER, L"Save folder", text, ARRAYSIZE(text)));

    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (settings::RunAtStartup() ? MF_CHECKED : MF_UNCHECKED),
                IDM_RUN_AT_STARTUP,
                LoadText(IDS_MENU_STARTUP, L"Run at startup", text, ARRAYSIZE(text)));
    // Debug builds say so in the menu. The two builds are identical from the
    // tray, so telling them apart otherwise means reading the process's memory
    // use, which is where a debug build's ASan shadow memory shows up. The
    // marker is not a STRINGTABLE entry: it is the same word in every language
    // and it does not exist in the shipped binary.
    LoadText(IDS_MENU_ABOUT, L"About", text, ARRAYSIZE(text));
#if defined(_DEBUG)
    wcscat_s(text, ARRAYSIZE(text), L" (Debug)");
#endif
    AppendMenuW(menu.get(), MF_STRING, IDM_ABOUT, text);

    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, IDM_RESTART,
                LoadText(IDS_MENU_RESTART, L"Restart", text, ARRAYSIZE(text)));
    AppendMenuW(menu.get(), MF_STRING, IDM_EXIT,
                LoadText(IDS_MENU_EXIT, L"Exit", text, ARRAYSIZE(text)));

    // Scaled by the monitor the menu opens on; the host window is a hidden
    // 0 x 0 and says nothing about where the tray is.
    UINT dpiX = 96;
    UINT dpiY = 96;
    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr) {
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    }
    // The spacers have to outlive the tracking, which is modal, so this scope
    // is exactly their lifetime.
    const int rowHeight = MulDiv(kMenuRowDip, static_cast<int>(dpiY), 96);
    const int markSide = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpiY);
    MenuSpacers spacers;
    spacers.blank = CreateBlankSpacer(rowHeight);
    spacers.radio = CreateMarkSpacer(rowHeight, markSide, DFCS_MENUBULLET);
    spacers.check = CreateMarkSpacer(rowHeight, markSide, DFCS_MENUCHECK);
    if (spacers.blank && spacers.radio && spacers.check) {
        ApplyMenuSpacers(menu.get(), spacers);
    }

    // Taking the foreground first is what lets a click outside the menu dismiss
    // it; posting WM_NULL afterwards is the other half of that same fix.
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screenPoint.x,
                     screenPoint.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

// Opens today's folder when there is one, and the root otherwise, since that
// is where the next capture will land.
void OpenCaptureFolder() {
    const std::wstring& root = settings::CaptureRoot();
    if (root.empty()) {
        return;
    }
    // With date folders off the next capture lands in the root, so that is the
    // folder to show even while older date folders still exist.
    if (settings::DateFolders()) {
        SYSTEMTIME now{};
        GetLocalTime(&now);

        wchar_t today[MAX_PATH];
        if (swprintf_s(today, L"%s\\%04u-%02u-%02u", root.c_str(), now.wYear, now.wMonth,
                       now.wDay) >= 0 &&
            GetFileAttributesW(today) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(nullptr, L"open", today, nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    // Before the first capture of the day the root may not exist yet.
    SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ChangeCaptureFolder(HWND owner) {
    auto dialog = wil::CoCreateInstanceNoThrow<IFileOpenDialog>(CLSID_FileOpenDialog);
    if (!dialog) {
        SC_LOG(L"[settings] could not create the folder picker dialog.");
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        // FORCEFILESYSTEM keeps the result to somewhere that has a real path;
        // without it a virtual shell location can be chosen and then not
        // written to.
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }

    wchar_t text[256];
    dialog->SetTitle(
        LoadText(IDS_FOLDER_PICK_TITLE, L"Choose where captures are saved", text,
                 ARRAYSIZE(text)));

    // Start where captures currently go, so this reads as changing the folder
    // rather than choosing one from nothing.
    wil::com_ptr_nothrow<IShellItem> start;
    if (SUCCEEDED(SHCreateItemFromParsingName(settings::CaptureRoot().c_str(), nullptr,
                                              IID_PPV_ARGS(&start)))) {
        dialog->SetFolder(start.get());
    }

    // The owner window is hidden, so the dialog needs help reaching the front.
    SetForegroundWindow(owner);
    if (FAILED(dialog->Show(owner))) {
        return;  // cancelled
    }

    wil::com_ptr_nothrow<IShellItem> item;
    wil::unique_cotaskmem_string path;
    if (FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return;
    }
    settings::SetCaptureRoot(path.get());
}

// The same failure is not reported again inside this window.
//
// Whatever breaks a save (a folder that is gone, a full disk, a denied
// permission) breaks the next one too, and a capture takes well under a second
// here. Without this, a broken folder turns a burst of captures into a queue of
// balloons that keeps appearing long after the user has stopped. Telling them
// once is the whole value; repeating it is only noise.
//
// A different failure reports immediately, so a new problem is never hidden
// behind an old one.
constexpr ULONGLONG kFailureRepeatMs = 30000;
// A different failure reports without waiting out the 30 seconds, but never
// within this gap of the last balloon: a burst of captures against a broken
// target would otherwise stack balloons faster than anyone reads them.
constexpr ULONGLONG kBalloonGapMs = 5000;
ULONGLONG g_lastFailureAt = 0;
WPARAM g_lastFailureKind = 0;

// Says which part of the delivery failed, so the user knows whether the
// capture is lost or still sitting somewhere usable.
void ReportDeliveryFailure(WPARAM failures) {
    const ULONGLONG now = GetTickCount64();
    if (failures == g_lastFailureKind && now - g_lastFailureAt < kFailureRepeatMs) {
        SC_LOG(L"[notify] same failure %llu ms after the last; balloon skipped.",
               now - g_lastFailureAt);
        return;
    }
    if (now - g_lastFailureAt < kBalloonGapMs) {
        // Not recorded as shown: once the gap has passed, the next failure of
        // this kind still reports.
        SC_LOG(L"[notify] balloon %llu ms after the last; skipped for the gap.",
               now - g_lastFailureAt);
        return;
    }
    g_lastFailureKind = failures;
    g_lastFailureAt = now;

    // A diverted save: the file exists, but in the default folder rather than
    // the one the user configured. This must never pass silently, and the
    // balloon says where the file went, because nothing else does. When the
    // clipboard failed in the same capture, this still wins: both messages
    // agree the capture survived, and only this one carries the location,
    // which the user cannot discover on their own.
    if ((failures & kDeliverySaveFellBack) != 0) {
        wchar_t title[64];
        wchar_t format[192];
        wchar_t text[256];
        LoadText(IDS_ERR_SAVE_FALLBACK,
                 L"The configured save folder could not be used. The capture was saved to the "
                 L"default folder instead.\n%s",
                 format, ARRAYSIZE(format));
        const std::wstring where = ShortenPath(settings::DefaultCaptureRoot());
        if (swprintf_s(text, format, where.c_str()) < 0) {
            wcscpy_s(text, where.c_str());
        }
        g_tray.ShowBalloon(LoadText(IDS_ERR_FALLBACK_TITLE, L"Saved to the default folder",
                                    title, ARRAYSIZE(title)),
                           text);
        return;
    }

    UINT id = IDS_ERR_DELIVERY_ALL;
    if ((failures & kDeliveryCaptureFailed) != 0) {
        id = IDS_ERR_CAPTURE_NONE;
    } else if ((failures & kDeliverySaveFailed) != 0 &&
               (failures & kDeliveryClipboardFailed) == 0) {
        id = IDS_ERR_SAVE_ONLY;
    } else if ((failures & kDeliveryClipboardFailed) != 0 &&
               (failures & kDeliverySaveFailed) == 0) {
        id = IDS_ERR_CLIP_ONLY;
    }

    // Separate buffers: one call cannot fill the same one twice.
    wchar_t title[64];
    wchar_t text[256];
    g_tray.ShowBalloon(
        LoadText(IDS_ERR_CAPTURE_TITLE, L"Capture failed", title, ARRAYSIZE(title)),
        LoadText(id, L"The capture could not be delivered.", text, ARRAYSIZE(text)));
}

// Plain text onto the clipboard. The capture path has its own clipboard code,
// but that one carries bitmaps and runs on the worker thread; this is the
// two-line text case.
void CopyTextToClipboard(HWND owner, const wchar_t* text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* locked = GlobalLock(memory);
        if (locked != nullptr) {
            memcpy(locked, text, bytes);
            GlobalUnlock(memory);
            // The clipboard owns the memory once SetClipboardData succeeds.
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

// The copy button sits in the button row next to OK. TaskDialog has no way to
// put a control beside a line of content, and a DIALOGEX that could would take
// on theme and dark mode by hand.
constexpr int kAboutCopyButtonId = 100;

// Content to swap in after the copy button is pressed: the same lines with
// the author line marked copied.
struct AboutContent {
    const wchar_t* copied;
};

// Handles the copy button and hyperlink clicks; without a handler the links
// would render but do nothing. Copying goes to the clipboard rather than a
// mailto link on purpose: PCs without a configured mail client would open
// nothing, while the clipboard always works.
HRESULT CALLBACK AboutDialogCallback(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     LONG_PTR refData) {
    if (msg == TDN_BUTTON_CLICKED && wparam == kAboutCopyButtonId) {
        CopyTextToClipboard(hwnd, SWEEPCAP_AUTHOR_EMAIL_W);
        const auto* content = reinterpret_cast<const AboutContent*>(refData);
        // TDM_SET_ELEMENT_TEXT rather than TDM_UPDATE_ELEMENT_TEXT: the line
        // grows, and only the former lets the dialog take a new size.
        SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT,
                     reinterpret_cast<LPARAM>(content->copied));
        return S_FALSE;  // keep the dialog open
    }
    if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(lparam), nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

// The About box is a task dialog rather than a DIALOGEX: it follows the
// system theme, DPI and font on its own, and the content is a few fixed lines.
//
// The name and version come from the same macros the version resource uses,
// so the box cannot drift from the binary's metadata.
void ShowAboutDialog(HWND owner) {
    wchar_t title[64];
    wchar_t author[256];
    wchar_t authorCopied[256];
    wchar_t copyLabel[64];
    wchar_t github[256];
    wchar_t content[512];
    wchar_t contentCopied[512];
    wchar_t thirdParty[256];

    LoadText(IDS_ABOUT_AUTHOR, L"Author: " SWEEPCAP_AUTHOR_EMAIL_W, author,
             ARRAYSIZE(author));
    LoadText(IDS_ABOUT_AUTHOR_COPIED, L"Author: " SWEEPCAP_AUTHOR_EMAIL_W L" (copied)",
             authorCopied, ARRAYSIZE(authorCopied));
    // The GitHub line points at the maker's profile, not this repository: the
    // profile lists the maker's public tools, so it doubles as the link to
    // them.
    LoadText(IDS_ABOUT_GITHUB,
             L"GitHub: <a href=\"https://github.com/legendsteel11\">github.com/legendsteel11</a>",
             github, ARRAYSIZE(github));
    if (swprintf_s(content, L"%s\n%s", author, github) < 0) {
        wcscpy_s(content, author);
    }
    if (swprintf_s(contentCopied, L"%s\n%s", authorCopied, github) < 0) {
        wcscpy_s(contentCopied, authorCopied);
    }
    AboutContent swap{contentCopied};

    const TASKDIALOG_BUTTON copyButton{
        kAboutCopyButtonId,
        LoadText(IDS_ABOUT_COPY_EMAIL, L"Copy email address", copyLabel,
                 ARRAYSIZE(copyLabel))};

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
    config.pfCallback = AboutDialogCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&swap);
    config.pButtons = &copyButton;
    config.cButtons = 1;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    // Enter still closes; the copy button is the extra, not the default.
    config.nDefaultButton = IDOK;
    config.pszWindowTitle = LoadText(IDS_MENU_ABOUT, L"About", title, ARRAYSIZE(title));
    config.pszMainIcon = MAKEINTRESOURCEW(IDI_APPICON);
    // The version number stays put between releases while builds change
    // daily, so the commit hash is what actually identifies this binary.
    config.pszMainInstruction =
        SWEEPCAP_NAME_W L" v" SWEEPCAP_VERSION_W L" (" SWEEPCAP_COMMIT_W L")";
    config.pszContent = content;
    config.pszFooter =
        LoadText(IDS_ABOUT_THIRDPARTY, L"This application includes Microsoft WIL (MIT License).",
                 thirdParty, ARRAYSIZE(thirdParty));

    // The owner window is hidden, so the dialog needs help reaching the front.
    SetForegroundWindow(owner);
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

// Launches a fresh instance and shuts this one down. The tray's escape hatch:
// whatever state the hooks or the session have got into, a restart clears it
// without hunting for the process.
//
// The new instance starts while this one is still shutting down, so it is
// launched with --restart, which makes its single-instance check wait for the
// mutex instead of giving up.
void RestartApplication(HWND hwnd) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) {
        SC_LOG(L"[restart] GetModuleFileName failed err=%lu", GetLastError());
        return;
    }
    wchar_t command[MAX_PATH + 16];
    if (swprintf_s(command, L"\"%s\" --restart", path) < 0) {
        return;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wil::unique_process_information process;
    if (!CreateProcessW(path, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        SC_LOG(L"[restart] CreateProcess failed err=%lu", GetLastError());
        return;
    }
    SC_LOG(L"[restart] new instance launched; shutting down");
    DestroyWindow(hwnd);
}

void ToggleRunAtStartup(HWND owner) {
    const bool next = !settings::RunAtStartup();
    if (settings::SetRunAtStartup(next)) {
        SC_LOG(L"[settings] run at startup %s", next ? L"enabled" : L"disabled");
        return;
    }
    wchar_t text[256];
    MessageBoxW(owner,
                LoadText(IDS_ERR_STARTUP, L"Could not change the startup entry.", text,
                         ARRAYSIZE(text)),
                SWEEPCAP_NAME_W, MB_OK | MB_ICONWARNING);
}

// Modifier watch.
//
// No keyboard hook is installed, so the modifiers are polled here with
// GetAsyncKeyState. The timer runs only while the mouse has moved recently and
// stops itself once things go quiet, so nothing wakes up while the application
// sits idle in the tray.
constexpr UINT_PTR kModifierTimerId = 2;
constexpr UINT kModifierTimerMs = 40;
constexpr ULONGLONG kIdleStopMs = 2000;        // stop the timer after this much silence
constexpr ULONGLONG kPrewarmRefreshMs = 1500;  // a held modifier lets the frame go stale

bool g_modifierTimerRunning = false;
bool g_modifiersHeld = false;
ULONGLONG g_lastMouseActivity = 0;
ULONGLONG g_lastPrewarm = 0;

void PollModifiers(HWND hwnd) {
    const ULONGLONG now = GetTickCount64();
    const bool held = settings::ModifiersHeld();

    // Nothing is pre-grabbed during a window drag. The mouse button belongs to
    // that drag, so no capture can start, and window snapping asks for the
    // modifier to be held for the length of the drag: refreshing a full-screen
    // grab every 1.5 s through it costs 65 ms of a core and 32 MB of traffic
    // each time on a 4K screen, for a frame that will never be used.
    const bool draggingWindow = winsnap::WindowDragActive();

    if (held != g_modifiersHeld) {
        g_modifiersHeld = held;
        g_lastPrewarm = now;
        if (held && !draggingWindow) {
            g_session.Prewarm();
        } else if (!held) {
            g_session.DropPrewarm();
        }
    } else if (held && !draggingWindow && now - g_lastPrewarm >= kPrewarmRefreshMs) {
        g_lastPrewarm = now;
        g_session.Prewarm();
    }

    if (!held && now - g_lastMouseActivity > kIdleStopMs) {
        KillTimer(hwnd, kModifierTimerId);
        g_modifierTimerRunning = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        g_tray.Restore();
        return 0;
    }

    switch (msg) {
        // Capture messages from the hook. The callback only sets state and
        // posts; all the real work happens here.
        case hook::WM_SC_DRAG_BEGIN:
            g_session.Begin(hwnd);
            return 0;

        case hook::WM_SC_DRAG_UPDATE:
            g_session.Update();
            return 0;

        case hook::WM_SC_DRAG_END:
            g_session.Finish(hwnd);
            return 0;

        case hook::WM_SC_DRAG_CANCEL:
            g_session.Cancel(L"right click");
            return 0;

        // The mouse moved: keep the modifier-watch timer alive.
        case hook::WM_SC_MOUSE_ACTIVE:
            g_lastMouseActivity = GetTickCount64();
            if (!g_modifierTimerRunning) {
                SetTimer(hwnd, kModifierTimerId, kModifierTimerMs, nullptr);
                g_modifierTimerRunning = true;
            }
            return 0;

        case WM_TIMER:
            if (wparam == kEscapeTimerId) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    hook::CancelDrag();
                    g_session.Cancel(L"ESC");
                    return 0;
                }
                // Shift changes and the whole-window highlight both have to
                // land even while the mouse is standing still.
                g_session.Tick();
                return 0;
            }
            if (wparam == kModifierTimerId) {
                PollModifiers(hwnd);
                return 0;
            }
            if (wparam == kFlashTimerId) {
                g_session.EndFlash();
                return 0;
            }
            break;

        case kTrayCallbackMessage: {
            // NOTIFYICON_VERSION_4: wParam carries screen coordinates and the
            // low word of lParam carries the event.
            const UINT event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU) {
                const POINT pt{static_cast<LONG>(GET_X_LPARAM(wparam)),
                               static_cast<LONG>(GET_Y_LPARAM(wparam))};
                ShowTrayMenu(hwnd, pt);
            } else if (event == WM_LBUTTONDBLCLK) {
                // Matches the item the menu marks as its default. A single
                // click does nothing, so brushing the icon has no effect.
                OpenCaptureFolder();
            }
            return 0;
        }

        case WM_SC_DELIVERY_FAILED:
            ReportDeliveryFailure(wparam);
            return 0;

        case WM_COMMAND: {
            // The radio submenus carry their index in the command id, so the
            // handler is a range test and a subtraction.
            const UINT id = LOWORD(wparam);
            if (id >= IDM_GESTURE_FIRST && id <= IDM_GESTURE_LAST) {
                settings::SetGestureIndex(static_cast<int>(id - IDM_GESTURE_FIRST));
                return 0;
            }
            if (id >= IDM_GRID_FIRST && id <= IDM_GRID_LAST) {
                settings::SetGridIndex(static_cast<int>(id - IDM_GRID_FIRST));
                return 0;
            }
            if (id >= IDM_WGRID_FIRST && id <= IDM_WGRID_LAST) {
                settings::SetWindowGridIndex(static_cast<int>(id - IDM_WGRID_FIRST));
                return 0;
            }
            if (id >= IDM_DIM_FIRST && id <= IDM_DIM_LAST) {
                settings::SetDimIndex(static_cast<int>(id - IDM_DIM_FIRST));
                return 0;
            }
            switch (id) {
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_OPEN_FOLDER:
                    OpenCaptureFolder();
                    return 0;
                case IDM_FOLDER_CHANGE:
                    ChangeCaptureFolder(hwnd);
                    return 0;
                case IDM_FOLDER_DEFAULT:
                    settings::SetCaptureRoot(nullptr);
                    return 0;
                case IDM_FOLDER_NO_DATE:
                    settings::SetDateFolders(!settings::DateFolders());
                    return 0;
                case IDM_RUN_AT_STARTUP:
                    ToggleRunAtStartup(hwnd);
                    return 0;
                case IDM_ABOUT:
                    ShowAboutDialog(hwnd);
                    return 0;
                case IDM_RESTART:
                    RestartApplication(hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        }

        // Instrumentation: re-dump the coordinates whenever the monitor
        // configuration changes.
        case WM_DISPLAYCHANGE:
            g_session.Cancel(L"display change");
            LogDesktopGeometry(L"WM_DISPLAYCHANGE");
            return 0;

        case WM_DPICHANGED:
            SC_LOG(L"WM_DPICHANGED: dpi=%u", LOWORD(wparam));
            LogDesktopGeometry(L"WM_DPICHANGED");
            return 0;

        case WM_DESTROY:
            g_session.Cancel(L"exit");
            hook::Remove();
            winsnap::Remove();
            g_session.Shutdown();
            g_tray.Remove();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The tray-resident window.
//
// A message-only window (HWND_MESSAGE) looks like the obvious choice and is the
// wrong one: broadcasts such as TaskbarCreated only reach top-level windows.
// So this is a real top-level window that is never shown, with WS_EX_TOOLWINDOW
// keeping it out of the taskbar and Alt+Tab.
HWND CreateHostWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = SWEEPCAP_WNDCLASS_W;
    if (RegisterClassExW(&wc) == 0) {
        SC_LOG(L"RegisterClassEx failed err=%lu", GetLastError());
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, SWEEPCAP_WNDCLASS_W, SWEEPCAP_NAME_W,
                                WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance,
                                nullptr);
    if (hwnd == nullptr) {
        SC_LOG(L"CreateWindowEx failed err=%lu", GetLastError());
    }
    return hwnd;
}

// Forces the UI language for this run:
//
//     SweepCap.exe --lang=en-US
//
// The resource script carries one STRINGTABLE per language and the loader
// picks by the user's UI language, so the other one is otherwise only visible
// on a system whose display language is set to it. This makes it checkable
// without a second machine.
//
// Has to run before anything loads a string, and the preference is set for the
// process and this thread both: the resource loader reads the thread list, and
// the process list is what any later thread inherits.
void ApplyLanguageOverride() {
    const wchar_t* argument = wcsstr(GetCommandLineW(), L"--lang=");
    if (argument == nullptr) {
        return;
    }
    argument += wcslen(L"--lang=");

    // MUI_LANGUAGE_NAME takes a double null terminated list. The buffer is
    // zeroed and the copy stops short of its end, so the terminators are there.
    wchar_t name[LOCALE_NAME_MAX_LENGTH + 2]{};
    size_t length = 0;
    while (length < LOCALE_NAME_MAX_LENGTH && argument[length] != L'\0' &&
           argument[length] != L' ') {
        name[length] = argument[length];
        ++length;
    }
    if (length == 0) {
        return;
    }

    ULONG count = 0;
    const BOOL processOk = SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, name, &count);
    count = 0;
    const BOOL threadOk = SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, name, &count);
    SC_LOG(L"[lang] override %s process=%d thread=%d", name, processOk, threadOk);
}

}  // namespace

int Run(HINSTANCE instance) {
    // Single instance. A second launch withdraws quietly.
    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, SWEEPCAP_MUTEX_W)};
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    log::Init();
    SC_LOG(L"%s %s (%s) starting", SWEEPCAP_NAME_W, SWEEPCAP_VERSION_W, SWEEPCAP_COMMIT_W);
    ApplyLanguageOverride();

    // A restart takes over from an instance that is still shutting down, so
    // the check is retried for a few seconds instead of giving up on the
    // first try.
    if (alreadyRunning && wcsstr(GetCommandLineW(), L"--restart") != nullptr) {
        for (int i = 0; i < 50 && alreadyRunning; ++i) {
            Sleep(100);
            HANDLE retry = CreateMutexW(nullptr, TRUE, SWEEPCAP_MUTEX_W);
            // Read the error before reset(): closing the previous handle could
            // overwrite it, and a lost ERROR_ALREADY_EXISTS here would let two
            // instances run at once.
            const DWORD error = GetLastError();
            single.reset(retry);
            alreadyRunning = (error == ERROR_ALREADY_EXISTS);
        }
        // The old instance held the log file without write sharing, so the
        // open in log::Init lost the race whenever the two overlapped; now
        // that the old instance is gone the file can be taken over too.
        if (!alreadyRunning) {
            log::Reopen();
        }
        SC_LOG(L"[restart] previous instance %s",
               alreadyRunning ? L"still running; giving up" : L"gone; taking over");
    }

    if (alreadyRunning) {
        SC_LOG(L"already running; exiting.");
        return 0;
    }

    // WIC (PNG encoding) and the shell APIs need COM.
    const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);

    // Before the hook goes in: the gesture it tests for comes from here.
    settings::Load();

    // The manifest already declares Per-Monitor V2, but record what actually
    // took effect. Without this, a manifest that failed to apply would show up
    // much later as coordinates that are subtly wrong.
    {
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(ctx);
        const bool isPerMonitorV2 =
            AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) !=
            FALSE;
        SC_LOG(L"DPI awareness: awareness=%d, PerMonitorV2=%s", static_cast<int>(awareness),
               isPerMonitorV2 ? L"yes" : L"no (check the manifest)");
    }

    LogDesktopGeometry(L"startup");

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreated == 0) {
        SC_LOG(L"RegisterWindowMessage(TaskbarCreated) failed err=%lu", GetLastError());
    }

    HWND hwnd = CreateHostWindow(instance);
    if (hwnd == nullptr) {
        log::Shutdown();
        return 1;
    }

    wchar_t message[512];

    if (!g_tray.Add(hwnd, kTrayCallbackMessage, kTrayIconId)) {
        // The tray icon is the only entry point, so there is no way for the
        // user to reach the application if this fails.
        MessageBoxW(nullptr,
                    LoadText(IDS_ERR_TRAY, L"Could not register the tray icon.", message,
                             ARRAYSIZE(message)),
                    SWEEPCAP_NAME_W, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        log::Shutdown();
        return 1;
    }

    g_session.Init(hwnd);

    if (!hook::Install(hwnd)) {
        // Region capture is dead without the hook, but the tray icon stays so
        // the user can still exit.
        MessageBoxW(nullptr,
                    LoadText(IDS_ERR_HOOK,
                             L"Could not install the mouse hook.\nRegion capture is disabled.",
                             message, ARRAYSIZE(message)),
                    SWEEPCAP_NAME_W, MB_ICONWARNING | MB_OK);
    }

    // Window snapping is independent of the mouse hook: it runs off window
    // events, so losing one does not take the other with it.
    winsnap::Install();

    SC_LOG(L"resident in the tray");
    // Baseline to compare every later reading against.
    SC_LOG_RESOURCES(L"startup");

    MSG msg{};
    BOOL got = 0;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) {
            SC_LOG(L"GetMessage failed err=%lu", GetLastError());
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SC_LOG(L"exit (code %d)", static_cast<int>(msg.wParam));
    log::Shutdown();
    return static_cast<int>(msg.wParam);
}

}  // namespace sc
