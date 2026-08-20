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
#include "settings.h"
#include "windowpick.h"

namespace sc {
namespace {

// A pre-grabbed frame older than this is discarded and taken again. The
// modifier watch refreshes every 1.5 s, so a normal flow never reaches it.
constexpr ULONGLONG kPrewarmMaxAgeMs = 3000;

// Handoff slot between the worker thread and the UI thread.
std::mutex g_prewarmMutex;
std::unique_ptr<FrozenFrame> g_prewarmed;
volatile LONG g_prewarmBusy = 0;
PTP_WORK g_prewarmWork = nullptr;

// Elapsed-time helper for instrumentation.
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

// Rounds to the nearest grid line, working from an origin that can be negative.
LONG SnapToGrid(LONG value, LONG origin, LONG pitch) {
    const LONG delta = value - origin;
    // Floor division, so negative offsets round the same way as positive ones.
    LONG cells = delta / pitch;
    const LONG remainder = delta % pitch;
    if (remainder != 0 && ((remainder < 0) != (pitch < 0))) {
        --cells;
    }
    const LONG lower = origin + cells * pitch;
    return (value - lower) * 2 >= pitch ? lower + pitch : lower;
}

// The grid cell a point falls in.
RECT CellAt(POINT pt, POINT origin, LONG pitch) {
    const auto floorDiv = [pitch](LONG v) {
        LONG q = v / pitch;
        if (v % pitch != 0 && v < 0) {
            --q;
        }
        return q;
    };
    const LONG cx = floorDiv(pt.x - origin.x);
    const LONG cy = floorDiv(pt.y - origin.y);
    return RECT{origin.x + cx * pitch, origin.y + cy * pitch, origin.x + (cx + 1) * pitch,
                origin.y + (cy + 1) * pitch};
}

// Top-left of the monitor a point sits on. Snapping is anchored here rather
// than to the virtual desktop origin, because monitors can start at offsets
// that are not multiples of the grid pitch.
POINT MonitorOriginFor(POINT pt) {
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return POINT{info.rcMonitor.left, info.rcMonitor.top};
    }
    return POINT{0, 0};
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

// Takes the pre-grabbed frame if it is still fresh, leaving the slot empty.
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
        // Wait for the worker before tearing anything down, otherwise shutdown
        // can touch a slot that is already gone.
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
    // Create the overlay window ahead of the click as well.
    overlay_.Prepare();

    // Do not stack requests while one is already running.
    if (InterlockedCompareExchange(&g_prewarmBusy, 1, 0) != 0) {
        return;
    }
    SubmitThreadpoolWork(g_prewarmWork);
}

void CaptureSession::DropPrewarm() {
    // The modifier was released; there is no reason to keep tens of megabytes
    // of screen data resident.
    std::lock_guard lock(g_prewarmMutex);
    g_prewarmed.reset();
}

bool CaptureSession::WindowLatched() const { return hasPick_ && !pickReleased_; }

bool CaptureSession::WindowShown() const {
    return WindowLatched() &&
           GetTickCount64() - dragStartedAt_ >= kWindowHighlightDelayMs;
}

void CaptureSession::UpdatePickRelease() {
    if (!hasPick_ || pickReleased_) {
        return;
    }
    const POINT anchor = hook::Anchor();
    const POINT current = hook::Current();
    const double dx = static_cast<double>(current.x - anchor.x);
    const double dy = static_cast<double>(current.y - anchor.y);
    // Once this is a drag it stays a drag; coming back inside the threshold
    // must not resurrect the window, or a careful small selection would turn
    // into a whole window on release.
    if (std::sqrt(dx * dx + dy * dy) >= config::kMinDragPixels) {
        pickReleased_ = true;
    }
}

RECT CaptureSession::CurrentSelection() const {
    return WindowShown() ? pick_.frame : RawOrSnapped();
}

RECT CaptureSession::RawOrSnapped() const {
    const RECT raw = MakeRect(hook::Anchor(), hook::Current());
    if (!snapEnabled_) {
        return raw;
    }

    const LONG pitch = settings::GridSizePx();
    RECT snapped{SnapToGrid(raw.left, gridOrigin_.x, pitch),
                 SnapToGrid(raw.top, gridOrigin_.y, pitch),
                 SnapToGrid(raw.right, gridOrigin_.x, pitch),
                 SnapToGrid(raw.bottom, gridOrigin_.y, pitch)};

    // Rounding both edges to the nearest line can collapse a short drag to
    // nothing. Keep at least one cell, growing in the direction of the drag.
    if (snapped.right == snapped.left) {
        if (hook::Current().x < hook::Anchor().x) {
            snapped.left -= pitch;
        } else {
            snapped.right += pitch;
        }
    }
    if (snapped.bottom == snapped.top) {
        if (hook::Current().y < hook::Anchor().y) {
            snapped.top -= pitch;
        } else {
            snapped.bottom += pitch;
        }
    }
    return snapped;
}

void CaptureSession::Tick() {
    if (!active_) {
        return;
    }
    UpdatePickRelease();
    const bool held = settings::SnapModifierHeld();
    const bool shown = WindowShown();
    if (held == snapEnabled_ && shown == windowShownLast_) {
        return;
    }
    snapEnabled_ = held;
    windowShownLast_ = shown;
    overlay_.SetSelection(CurrentSelection(), shown);
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

    snapEnabled_ = settings::SnapModifierHeld();
    gridOrigin_ = MonitorOriginFor(anchor);
    startCell_ = CellAt(anchor, gridOrigin_, settings::GridSizePx());

    // Find what a release would capture. Three rules in order: the window under
    // the cursor, then a window with an exposed corner in the starting cell,
    // then the whole monitor. The corner rule has to come before the monitor
    // one, otherwise bare desktop would always mean full screen and the case it
    // exists for - a window buried except for one corner - would never fire.
    //
    // All of this has to run before the overlay goes up. The exposure test asks
    // WindowFromPoint what owns a pixel, and once the overlay covers the screen
    // the answer is always the overlay.
    hasPick_ = false;
    pickReleased_ = false;
    pick_ = WindowPick{};
    {
        const Stopwatch pickWatch;
        const wchar_t* rule = L"커서 아래 창";
        hasPick_ = PickWindowAt(anchor, &pick_);
        if (!hasPick_ && snapEnabled_) {
            rule = L"시작 칸의 창 모서리";
            hasPick_ = PickWindowByCorner(startCell_, &pick_);
        }
        if (!hasPick_) {
            rule = L"모니터 전체";
            hasPick_ = PickMonitorAt(anchor, &pick_);
        }
        pickRule_ = hasPick_ ? rule : nullptr;
        // Named here rather than at the end of the drag: the window may be
        // gone by then, and this is the same place that already has to run
        // before the overlay covers the screen. A free drag keeps the name of
        // the window it started over.
        appName_ = AppNameForWindow(pick_.hwnd);
        if (hasPick_) {
            SC_LOG(L"[세션] 잡을 대상 (%s, %.2f ms) %ldx%ld 모서리 반지름 %d", rule,
                   pickWatch.ElapsedMs(), pick_.frame.right - pick_.frame.left,
                   pick_.frame.bottom - pick_.frame.top, pick_.cornerRadius);
        }
    }

    if (!frame_->AttachDc() || !overlay_.Show(*frame_, anchor)) {
        SC_LOG(L"[세션] 오버레이 표시 실패. 접는다.");
        hook::CancelDrag();
        Teardown();
        return;
    }

    dragStartedAt_ = GetTickCount64();
    windowShownLast_ = false;

    active_ = true;
    SetTimer(owner_, kEscapeTimerId, kEscapeTimerMs, nullptr);
    SC_LOG(L"[세션] 오버레이까지 %.2f ms", watch.ElapsedMs());
}

void CaptureSession::Update() {
    hook::AcknowledgeUpdate();
    if (!active_) {
        return;
    }
    UpdatePickRelease();
    snapEnabled_ = settings::SnapModifierHeld();
    windowShownLast_ = WindowShown();
    overlay_.SetSelection(CurrentSelection(), windowShownLast_);
}

void CaptureSession::Finish(HWND owner) {
    if (!active_) {
        return;
    }

    UpdatePickRelease();
    const bool windowPick = WindowLatched();
    // Uses the pick even when the highlight never had time to appear, so a
    // quick click still captures the window.
    const RECT selection = windowPick ? pick_.frame : RawOrSnapped();
    const POINT anchor = hook::Anchor();
    const POINT current = hook::Current();
    const double dx = static_cast<double>(current.x - anchor.x);
    const double dy = static_cast<double>(current.y - anchor.y);
    const double distance = std::sqrt(dx * dx + dy * dy);

    // Take the overlay down first. Cropping reads the frozen frame either way,
    // but clearing the screen sooner is what makes it feel immediate.
    overlay_.Hide();

    // A whole-window pick is deliberate even without any movement, so the
    // accidental-trigger threshold does not apply to it.
    if (!windowPick && distance < config::kMinDragPixels) {
        SC_LOG(L"[세션] 드래그가 %.0fpx뿐이다 (최소 %d). 취소한다.", distance,
               config::kMinDragPixels);
        Teardown();
        return;
    }

    const Stopwatch cropWatch;
    Bitmap32 shot = frame_->Crop(selection);
    if (windowPick) {
        // Windows 11 rounds window corners, so a straight rectangular crop
        // brings the background along in all four of them.
        CarveRoundedCorners(shot, pick_.cornerRadius);
    }
    if (!shot.Valid()) {
        SC_LOG(L"[세션] 잘라내기 실패 rect=(%ld,%ld,%ld,%ld)", selection.left, selection.top,
               selection.right, selection.bottom);
        if (host_ != nullptr) {
            PostMessageW(host_, WM_SC_DELIVERY_FAILED, kDeliveryCaptureFailed, 0);
        }
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
        saveOk = SavePng(png, appName_, path);
        saveMs = watch.ElapsedMs();
    }

    SC_LOG(L"[세션] 완료 %dx%d (%s)  잘라내기 %.2f / 인코딩 %.2f / 클립보드 %.2f / 저장 %.2f ms",
           shot.width, shot.height,
           windowPick ? pickRule_ : (snapEnabled_ ? L"격자" : L"자유"), cropMs, encodeMs,
           clipboardMs, saveMs);
    SC_LOG(L"[세션] PNG %zu bytes, 클립보드=%s, 저장=%s %s", png.size(),
           clipboardOk ? L"성공" : L"실패", saveOk ? L"성공" : L"실패",
           saveOk ? path.c_str() : L"");

    // Say so when a capture did not arrive anywhere. Release builds compile
    // every SC_LOG away, so without this a failed save is indistinguishable
    // from a successful one: the overlay disappears either way.
    WPARAM failures = 0;
    if (config::kCopyToClipboard && !clipboardOk) {
        failures |= kDeliveryClipboardFailed;
    }
    if (config::kSaveToFile && !saveOk) {
        failures |= kDeliverySaveFailed;
    }
    if (png.empty()) {
        failures |= kDeliveryCaptureFailed;
    }
    if (failures != 0 && host_ != nullptr) {
        PostMessageW(host_, WM_SC_DELIVERY_FAILED, failures, 0);
    }

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
    hasPick_ = false;
    pickReleased_ = false;
    windowShownLast_ = false;
    pickRule_ = nullptr;
    pick_ = WindowPick{};
    appName_.clear();
    active_ = false;
}

}  // namespace sc
