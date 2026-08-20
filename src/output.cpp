#include "output.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>

#include "app_name.h"
#include "capture.h"
#include "log.h"

namespace sc {
namespace {

// 클립보드에 올릴 PNG 포맷은 등록 포맷이다. 이름은 관례로 "PNG"다.
UINT PngClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

// HGLOBAL 하나를 만들어 바이트를 그대로 복사한다.
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

// CF_DIBV5 블록을 만든다.
//
// 높이를 양수로 두어 bottom-up으로 넣는다. top-down(음수 높이)도 규격상
// 맞지만 받는 앱 중에 뒤집어 붙이는 것들이 있다.
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
    header->bV5Height = bitmap.height;  // 양수 = bottom-up
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
        // 원본은 top-down이므로 마지막 행부터 넣는다.
        memcpy(dst + static_cast<size_t>(y) * rowPixels, bitmap.Row(bitmap.height - 1 - y),
               rowPixels * sizeof(uint32_t));
    }

    GlobalUnlock(block.get());
    return block;
}

// Pictures\SweepCap\<날짜> 폴더를 확보하고 경로를 돌려준다.
bool EnsureCaptureFolder(const SYSTEMTIME& now, std::wstring& outFolder) {
    wil::unique_cotaskmem_string pictures;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures);
    if (FAILED(hr)) {
        SC_LOG(L"[저장] SHGetKnownFolderPath(Pictures) 실패 hr=0x%08lX",
               static_cast<unsigned long>(hr));
        return false;
    }

    wchar_t folder[MAX_PATH];
    if (swprintf_s(folder, L"%s\\%s\\%04u-%02u-%02u", pictures.get(), SWEEPCAP_NAME_W,
                   now.wYear, now.wMonth, now.wDay) < 0) {
        return false;
    }

    const int created = SHCreateDirectoryExW(nullptr, folder, nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS &&
        created != ERROR_FILE_EXISTS) {
        SC_LOG(L"[저장] 폴더 생성 실패 (%d) %s", created, folder);
        return false;
    }

    outFolder.assign(folder);
    return true;
}

}  // namespace

std::vector<uint8_t> EncodePng(const Bitmap32& bitmap) {
    std::vector<uint8_t> out;
    if (!bitmap.Valid()) {
        return out;
    }

    auto factory = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
    if (!factory) {
        SC_LOG(L"[인코딩] WICImagingFactory 생성 실패");
        return out;
    }

    wil::com_ptr_nothrow<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        SC_LOG(L"[인코딩] CreateStreamOnHGlobal 실패");
        return out;
    }

    wil::com_ptr_nothrow<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        SC_LOG(L"[인코딩] PNG 인코더 초기화 실패");
        return out;
    }

    wil::com_ptr_nothrow<IWICBitmapFrameEncode> frame;
    wil::com_ptr_nothrow<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.get()))) {
        SC_LOG(L"[인코딩] 프레임 초기화 실패");
        return out;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(static_cast<UINT>(bitmap.width),
                              static_cast<UINT>(bitmap.height))) ||
        FAILED(frame->SetPixelFormat(&format))) {
        SC_LOG(L"[인코딩] 프레임 설정 실패");
        return out;
    }
    if (format != GUID_WICPixelFormat32bppBGRA) {
        SC_LOG(L"[인코딩] 인코더가 32bppBGRA를 받지 않았다");
        return out;
    }

    const UINT stride = static_cast<UINT>(bitmap.width) * 4;
    // WritePixels는 const가 아닌 포인터를 받지만 내용을 바꾸지 않는다.
    auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bitmap.pixels.data()));
    if (FAILED(frame->WritePixels(static_cast<UINT>(bitmap.height), stride,
                                  static_cast<UINT>(bitmap.ByteSize()), pixels)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        SC_LOG(L"[인코딩] PNG 쓰기 실패");
        return out;
    }

    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.get(), &global)) || global == nullptr) {
        SC_LOG(L"[인코딩] GetHGlobalFromStream 실패");
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

    // 잠글 블록을 먼저 다 만들어 둔다. 클립보드를 연 상태를 짧게 유지한다.
    wil::unique_hglobal dib = MakeDibV5(bitmap);
    if (!dib) {
        SC_LOG(L"[클립보드] DIBV5 블록 생성 실패");
        return false;
    }
    wil::unique_hglobal pngBlock;
    if (!png.empty()) {
        pngBlock = MakeGlobal(png.data(), png.size());
    }

    auto clipboard = wil::open_clipboard(owner);
    if (!clipboard) {
        SC_LOG(L"[클립보드] OpenClipboard 실패 err=%lu", GetLastError());
        return false;
    }
    if (!EmptyClipboard()) {
        SC_LOG(L"[클립보드] EmptyClipboard 실패 err=%lu", GetLastError());
        return false;
    }

    bool any = false;
    // SetClipboardData가 성공하면 소유권이 클립보드로 넘어간다. release()로 놓는다.
    if (SetClipboardData(CF_DIBV5, dib.get()) != nullptr) {
        dib.release();
        any = true;
    } else {
        SC_LOG(L"[클립보드] CF_DIBV5 등록 실패 err=%lu", GetLastError());
    }

    const UINT pngFormat = PngClipboardFormat();
    if (pngBlock && pngFormat != 0) {
        if (SetClipboardData(pngFormat, pngBlock.get()) != nullptr) {
            pngBlock.release();
        } else {
            SC_LOG(L"[클립보드] PNG 등록 실패 err=%lu", GetLastError());
        }
    }
    return any;
}

bool SavePng(const std::vector<uint8_t>& png, std::wstring& outPath) {
    if (png.empty()) {
        return false;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    std::wstring folder;
    if (!EnsureCaptureFolder(now, folder)) {
        return false;
    }

    // 2026-08-18_17-23-33_451.png
    // 밀리초까지 넣으므로 사람 손으로는 겹칠 수 없다. 그래도 겹치면 -2, -3.
    wchar_t stem[64];
    if (swprintf_s(stem, L"%04u-%02u-%02u_%02u-%02u-%02u_%03u", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds) < 0) {
        return false;
    }

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

        // CREATE_NEW다. 이미 있으면 실패하고 다음 번호로 넘어간다.
        // 덮어쓰는 경로가 아예 없다.
        wil::unique_hfile file{CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!file) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            SC_LOG(L"[저장] 파일 생성 실패 err=%lu %s", error, path);
            return false;
        }

        DWORD wrote = 0;
        if (!WriteFile(file.get(), png.data(), static_cast<DWORD>(png.size()), &wrote, nullptr) ||
            wrote != png.size()) {
            SC_LOG(L"[저장] 쓰기 실패 err=%lu %s", GetLastError(), path);
            return false;
        }
        outPath.assign(path);
        return true;
    }

    SC_LOG(L"[저장] 이름이 계속 겹친다. 포기한다.");
    return false;
}

}  // namespace sc
