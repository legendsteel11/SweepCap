#include "session.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "hook.h"
#include "log.h"
#include "output.h"

namespace sc {
namespace {

// 미리 떠 둔 프레임이 이보다 묵었으면 버리고 다시 뜬다.
// 훅이 1.5초마다 갱신을 요청하므로 정상 흐름에서는 이 값에 안 닿는다.
constexpr ULONGLONG kPrewarmMaxAgeMs = 3000;

// 워커 스레드와 UI 스레드가 프레임을 주고받는 자리.
std::mutex g_prewarmMutex;
std::unique_ptr<FrozenFrame> g_prewarmed;
volatile LONG g_prewarmBusy = 0;
PTP_WORK g_prewarmWork = nullptr;

// 계측용 경과 시간.
class Stopwatch {
public:
    Stopwatch() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_);
    }
    double ElapsedMs() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 /
               static_cast<double>(freq_.QuadPart);
    }

private:
    LARGE_INTEGER freq_{};
    LARGE_INTEGER start_{};
};

RECT MakeRect(POINT a, POINT b) {
    return RECT{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

void CALLBACK PrewarmWork(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK) {
    auto frame = std::make_unique<FrozenFrame>();
    const bool ok = frame->GrabPixels();
    {
        std::lock_guard lock(g_prewarmMutex);
        g_prewarmed = ok ? std::move(frame) : nullptr;
    }
    InterlockedExchange(&g_prewarmBusy, 0);
}

// 쓸 만한(충분히 최근인) 프레임을 가져온다. 가져가면 자리는 비워진다.
std::unique_ptr<FrozenFrame> TakeFreshPrewarm(ULONGLONG* outAgeMs) {
    std::lock_guard lock(g_prewarmMutex);
    if (!g_prewarmed) {
        return nullptr;
    }
    const ULONGLONG age = GetTickCount64() - g_prewarmed->GrabbedAt();
    if (age > kPrewarmMaxAgeMs) {
        g_prewarmed.reset();
        return nullptr;
    }
    if (outAgeMs != nullptr) {
        *outAgeMs = age;
    }
    return std::move(g_prewarmed);
}

}  // namespace

void CaptureSession::Init(HWND host) {
    host_ = host;
    g_prewarmWork = CreateThreadpoolWork(PrewarmWork, nullptr, nullptr);
    if (g_prewarmWork == nullptr) {
        SC_LOG(L"[세션] CreateThreadpoolWork 실패 err=%lu. 미리 뜨기 없이 간다.",
               GetLastError());
    }
}

void CaptureSession::Shutdown() {
    if (g_prewarmWork != nullptr) {
        // 워커가 끝날 때까지 기다린 뒤에 정리한다. 안 기다리면 종료 중에
        // 이미 없어진 자리를 건드린다.
        WaitForThreadpoolWorkCallbacks(g_prewarmWork, TRUE);
        CloseThreadpoolWork(g_prewarmWork);
        g_prewarmWork = nullptr;
    }
    std::lock_guard lock(g_prewarmMutex);
    g_prewarmed.reset();
}

void CaptureSession::Prewarm() {
    if (active_ || g_prewarmWork == nullptr) {
        return;
    }
    // 창을 미리 만들어 둔다.
    overlay_.Prepare();

    // 이미 뜨는 중이면 겹쳐 던지지 않는다.
    if (InterlockedCompareExchange(&g_prewarmBusy, 1, 0) != 0) {
        return;
    }
    SubmitThreadpoolWork(g_prewarmWork);
}

void CaptureSession::DropPrewarm() {
    // 수식키를 놓았다. 32MB를 계속 들고 있을 이유가 없다.
    std::lock_guard lock(g_prewarmMutex);
    g_prewarmed.reset();
}

RECT CaptureSession::CurrentSelection() const {
    return MakeRect(hook::Anchor(), hook::Current());
}

void CaptureSession::Begin(HWND owner) {
    if (active_) {
        SC_LOG(L"[세션] 이미 진행 중인데 Begin이 왔다. 앞의 것을 접는다.");
        Teardown();
    }

    owner_ = owner;
    const POINT anchor = hook::Anchor();
    const Stopwatch watch;

    ULONGLONG age = 0;
    frame_ = TakeFreshPrewarm(&age);
    if (frame_) {
        SC_LOG(L"[세션] 시작 anchor=(%ld,%ld) 미리 떠 둔 프레임 사용 (%llu ms 전)", anchor.x,
               anchor.y, age);
    } else {
        SC_LOG(L"[세션] 시작 anchor=(%ld,%ld) 미리 떠 둔 것이 없어 지금 뜬다", anchor.x,
               anchor.y);
        frame_ = std::make_unique<FrozenFrame>();
        if (!frame_->GrabPixels()) {
            SC_LOG(L"[세션] 프리즈 프레임 실패. 접는다.");
            hook::CancelDrag();
            Teardown();
            return;
        }
    }

    if (!frame_->AttachDc() || !overlay_.Show(*frame_, anchor)) {
        SC_LOG(L"[세션] 오버레이 표시 실패. 접는다.");
        hook::CancelDrag();
        Teardown();
        return;
    }

    active_ = true;
    SetTimer(owner_, kEscapeTimerId, kEscapeTimerMs, nullptr);
    SC_LOG(L"[세션] 오버레이까지 %.2f ms", watch.ElapsedMs());
}

void CaptureSession::Update() {
    hook::AcknowledgeUpdate();
    if (!active_) {
        return;
    }
    overlay_.SetSelection(CurrentSelection());
}

void CaptureSession::Finish(HWND owner) {
    if (!active_) {
        return;
    }

    const RECT selection = CurrentSelection();
    const POINT anchor = hook::Anchor();
    const POINT current = hook::Current();
    const double dx = static_cast<double>(current.x - anchor.x);
    const double dy = static_cast<double>(current.y - anchor.y);
    const double distance = std::sqrt(dx * dx + dy * dy);

    // 선택 UI가 결과에 안 찍히게 먼저 내린다. 잘라내기는 어차피 프리즈
    // 프레임에서 하지만, 화면에서 빨리 사라지는 편이 빠르게 느껴진다.
    overlay_.Hide();

    if (distance < config::kMinDragPixels) {
        // 3단계에서 이 구간이 "창 fit 캡처"가 된다. 지금은 오발동으로 본다.
        SC_LOG(L"[세션] 드래그가 %.0fpx뿐이다 (최소 %d). 취소한다.", distance,
               config::kMinDragPixels);
        Teardown();
        return;
    }

    const Stopwatch cropWatch;
    const Bitmap32 shot = frame_->Crop(selection);
    if (!shot.Valid()) {
        SC_LOG(L"[세션] 잘라내기 실패 rect=(%ld,%ld,%ld,%ld)", selection.left, selection.top,
               selection.right, selection.bottom);
        Teardown();
        return;
    }
    const double cropMs = cropWatch.ElapsedMs();

    const Stopwatch encodeWatch;
    const std::vector<uint8_t> png = EncodePng(shot);
    const double encodeMs = encodeWatch.ElapsedMs();

    double clipboardMs = 0.0;
    bool clipboardOk = false;
    if (config::kCopyToClipboard) {
        const Stopwatch watch;
        clipboardOk = CopyToClipboard(owner, shot, png);
        clipboardMs = watch.ElapsedMs();
    }

    double saveMs = 0.0;
    bool saveOk = false;
    std::wstring path;
    if (config::kSaveToFile) {
        const Stopwatch watch;
        saveOk = SavePng(png, path);
        saveMs = watch.ElapsedMs();
    }

    SC_LOG(L"[세션] 완료 %dx%d  잘라내기 %.2f / 인코딩 %.2f / 클립보드 %.2f / 저장 %.2f ms",
           shot.width, shot.height, cropMs, encodeMs, clipboardMs, saveMs);
    SC_LOG(L"[세션] PNG %zu bytes, 클립보드=%s, 저장=%s %s", png.size(),
           clipboardOk ? L"성공" : L"실패", saveOk ? L"성공" : L"실패",
           saveOk ? path.c_str() : L"");

    Teardown();
}

void CaptureSession::Cancel(const wchar_t* reason) {
    if (!active_) {
        return;
    }
    SC_LOG(L"[세션] 취소 (%s)", reason);
    Teardown();
}

void CaptureSession::Teardown() {
    if (owner_ != nullptr) {
        KillTimer(owner_, kEscapeTimerId);
    }
    overlay_.Hide();
    frame_.reset();
    active_ = false;
}

}  // namespace sc
