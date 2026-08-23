#include "output.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>

#include "capture.h"
#include "log.h"
#include "settings.h"

namespace sc {
namespace {

// PNG on the clipboard is a registered format; "PNG" is the conventional name.
UINT PngClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

// Allocates one HGLOBAL and copies the bytes into it.
wil::unique_hglobal MakeGlobal(const void* data, size_t size) {
    wil::unique_hglobal block{GlobalAlloc(GMEM_MOVEABLE, size)};
    if (!block) {
        return {};
    }
    void* locked = GlobalLock(block.get());
    if (locked == nullptr) {
        return {};
    }
    memcpy(locked, data, size);
    GlobalUnlock(block.get());
    return block;
}

// Builds a CF_DIBV5 block.
//
// Height stays positive, i.e. bottom-up. Top-down (negative height) is legal
// too, but some receiving applications paste it upside down.
wil::unique_hglobal MakeDibV5(const Bitmap32& bitmap) {
    const size_t pixelBytes = bitmap.ByteSize();
    const size_t total = sizeof(BITMAPV5HEADER) + pixelBytes;

    wil::unique_hglobal block{GlobalAlloc(GMEM_MOVEABLE, total)};
    if (!block) {
        return {};
    }
    auto* base = static_cast<uint8_t*>(GlobalLock(block.get()));
    if (base == nullptr) {
        return {};
    }

    auto* header = reinterpret_cast<BITMAPV5HEADER*>(base);
    ZeroMemory(header, sizeof(*header));
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = bitmap.width;
    header->bV5Height = bitmap.height;  // positive: bottom-up
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_sRGB;
    header->bV5Intent = LCS_GM_GRAPHICS;

    auto* dst = reinterpret_cast<uint32_t*>(base + sizeof(BITMAPV5HEADER));
    const size_t rowPixels = static_cast<size_t>(bitmap.width);
    for (int y = 0; y < bitmap.height; ++y) {
        // The source is top-down, so write its rows in reverse.
        memcpy(dst + static_cast<size_t>(y) * rowPixels, bitmap.Row(bitmap.height - 1 - y),
               rowPixels * sizeof(uint32_t));
    }

    GlobalUnlock(block.get());
    return block;
}

// Stands in for the app name when there is no window behind the capture: a
// whole monitor, or a window whose process could not be read.
constexpr wchar_t kUnnamedApp[] = L"Screen";

// Makes sure the folder captures land in exists and returns its path: the
// per-date subfolder under the given root, or the root itself when date
// folders are turned off.
bool EnsureCaptureFolder(const std::wstring& root, const SYSTEMTIME& now,
                         std::wstring& outFolder) {
    if (root.empty()) {
        SC_LOG(L"[save] could not determine the save folder.");
        return false;
    }

    wchar_t folder[MAX_PATH];
    if (settings::DateFolders()) {
        if (swprintf_s(folder, L"%s\\%04u-%02u-%02u", root.c_str(), now.wYear, now.wMonth,
                       now.wDay) < 0) {
            return false;
        }
    } else if (swprintf_s(folder, L"%s", root.c_str()) < 0) {
        return false;
    }

    const int created = SHCreateDirectoryExW(nullptr, folder, nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS &&
        created != ERROR_FILE_EXISTS) {
        SC_LOG(L"[save] folder creation failed (%d) %s", created, folder);
        return false;
    }

    outFolder.assign(folder);
    return true;
}

// Writes the PNG bytes under one specific folder. Colliding names get -2, -3
// and so on; any other error gives up on this folder.
bool WriteUnderFolder(const std::wstring& folder, const wchar_t* stem,
                      const std::vector<uint8_t>& png, std::wstring& outPath) {
    for (int attempt = 1; attempt <= 99; ++attempt) {
        wchar_t path[MAX_PATH];
        int written = 0;
        if (attempt == 1) {
            written = swprintf_s(path, L"%s\\%s.png", folder.c_str(), stem);
        } else {
            written = swprintf_s(path, L"%s\\%s-%d.png", folder.c_str(), stem, attempt);
        }
        if (written < 0) {
            return false;
        }

        // CREATE_NEW: an existing file makes this fail and the loop moves to
        // the next number. There is no code path that overwrites.
        wil::unique_hfile file{CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!file) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            SC_LOG(L"[save] file creation failed err=%lu %s", error, path);
            return false;
        }

        DWORD wrote = 0;
        if (!WriteFile(file.get(), png.data(), static_cast<DWORD>(png.size()), &wrote, nullptr) ||
            wrote != png.size()) {
            SC_LOG(L"[save] write failed err=%lu %s", GetLastError(), path);
            return false;
        }
        outPath.assign(path);
        return true;
    }

    SC_LOG(L"[save] names keep colliding; giving up.");
    return false;
}

}  // namespace

std::vector<uint8_t> EncodePng(const Bitmap32& bitmap) {
    std::vector<uint8_t> out;
    if (!bitmap.Valid()) {
        return out;
    }

    auto factory = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
    if (!factory) {
        SC_LOG(L"[encode] WICImagingFactory creation failed");
        return out;
    }

    wil::com_ptr_nothrow<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        SC_LOG(L"[encode] CreateStreamOnHGlobal failed");
        return out;
    }

    wil::com_ptr_nothrow<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        SC_LOG(L"[encode] PNG encoder initialization failed");
        return out;
    }

    wil::com_ptr_nothrow<IWICBitmapFrameEncode> frame;
    wil::com_ptr_nothrow<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.get()))) {
        SC_LOG(L"[encode] frame initialization failed");
        return out;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(static_cast<UINT>(bitmap.width),
                              static_cast<UINT>(bitmap.height))) ||
        FAILED(frame->SetPixelFormat(&format))) {
        SC_LOG(L"[encode] frame setup failed");
        return out;
    }
    if (format != GUID_WICPixelFormat32bppBGRA) {
        SC_LOG(L"[encode] encoder rejected 32bppBGRA");
        return out;
    }

    const UINT stride = static_cast<UINT>(bitmap.width) * 4;
    // WritePixels takes a non-const pointer but does not modify the buffer.
    auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bitmap.pixels.data()));
    if (FAILED(frame->WritePixels(static_cast<UINT>(bitmap.height), stride,
                                  static_cast<UINT>(bitmap.ByteSize()), pixels)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        SC_LOG(L"[encode] PNG write failed");
        return out;
    }

    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.get(), &global)) || global == nullptr) {
        SC_LOG(L"[encode] GetHGlobalFromStream failed");
        return out;
    }
    const SIZE_T size = GlobalSize(global);
    const void* data = GlobalLock(global);
    if (data != nullptr && size > 0) {
        out.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        GlobalUnlock(global);
    }
    return out;
}

bool CopyToClipboard(HWND owner, const Bitmap32& bitmap, const std::vector<uint8_t>& png) {
    if (!bitmap.Valid()) {
        return false;
    }

    // Build every block before opening the clipboard, to hold it open briefly.
    wil::unique_hglobal dib = MakeDibV5(bitmap);
    if (!dib) {
        SC_LOG(L"[clipboard] DIBV5 block creation failed");
        return false;
    }
    wil::unique_hglobal pngBlock;
    if (!png.empty()) {
        pngBlock = MakeGlobal(png.data(), png.size());
    }

    auto clipboard = wil::open_clipboard(owner);
    if (!clipboard) {
        SC_LOG(L"[clipboard] OpenClipboard failed err=%lu", GetLastError());
        return false;
    }
    if (!EmptyClipboard()) {
        SC_LOG(L"[clipboard] EmptyClipboard failed err=%lu", GetLastError());
        return false;
    }

    bool any = false;
    // A successful SetClipboardData transfers ownership, so release the handle.
    if (SetClipboardData(CF_DIBV5, dib.get()) != nullptr) {
        dib.release();
        any = true;
    } else {
        SC_LOG(L"[clipboard] CF_DIBV5 set failed err=%lu", GetLastError());
    }

    const UINT pngFormat = PngClipboardFormat();
    if (pngBlock && pngFormat != 0) {
        if (SetClipboardData(pngFormat, pngBlock.get()) != nullptr) {
            pngBlock.release();
        } else {
            SC_LOG(L"[clipboard] PNG set failed err=%lu", GetLastError());
        }
    }
    return any;
}

SaveResult SavePng(const std::vector<uint8_t>& png, const std::wstring& appName,
                   std::wstring& outPath) {
    if (png.empty()) {
        return SaveResult::kFailed;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    // Chrome_2026-08-18_17-23-33.png
    //
    // The app name leads because reading runs left to right. When every name
    // opens with the same date, the only thing telling two of them apart is a
    // pair of digits in the middle, and a folder of those is slow to scan.
    // The date stays on so that a file dragged out of the folder into a chat
    // or a document still says when it was taken.
    //
    // Two captures of one app inside the same second collide; -2, -3 catch
    // that, and the app name keeps such a pair rare in the first place.
    wchar_t stem[96];
    if (swprintf_s(stem, L"%s_%04u-%02u-%02u_%02u-%02u-%02u",
                   appName.empty() ? kUnnamedApp : appName.c_str(), now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond) < 0) {
        return SaveResult::kFailed;
    }

    std::wstring folder;
    if (EnsureCaptureFolder(settings::CaptureRoot(), now, folder) &&
        WriteUnderFolder(folder, stem, png, outPath)) {
        return SaveResult::kSaved;
    }

    // A configured folder can stop working while the app runs: the drive is
    // unplugged, a permission is revoked, the path was mistyped. The capture
    // is not dropped for that; it goes to the default folder instead. The
    // setting is deliberately left alone, so the configured folder takes over
    // again the moment it works. Not the parent folder: that could be a drive
    // root, and it is nowhere the user ever chose.
    if (settings::CaptureRootIsDefault()) {
        return SaveResult::kFailed;
    }
    if (!EnsureCaptureFolder(settings::DefaultCaptureRoot(), now, folder) ||
        !WriteUnderFolder(folder, stem, png, outPath)) {
        return SaveResult::kFailed;
    }
    SC_LOG(L"[save] configured folder unusable; diverted to %s", outPath.c_str());
    return SaveResult::kSavedToDefault;
}

}  // namespace sc
