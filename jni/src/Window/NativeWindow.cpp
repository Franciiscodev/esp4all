#include "Window/NativeWindow.hpp"

#include "Window/LibGui.hpp"

#include <algorithm>
#include <android/log.h>
#include <android/native_window.h>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <utility>

namespace esp4all {

namespace {

constexpr const char* kLogTag = "ESP4AllWindow";
constexpr uint32_t kNativeWindowMagic = 0x5f776e64U; // '_wnd' (android_native_base_t::magic)

void SetError(std::string* outError, const std::string& msg) {
    if (outError) {
        *outError = msg;
    }
}

struct SurfaceFlingerSession {
    const LibGui* gui = nullptr;
    GuiPtr client = nullptr;
    GuiPtr token = nullptr;
    GuiPtr transaction = nullptr;
    uint64_t physicalDisplayId = 0;
    std::mutex txMutex;

    bool Connect(std::string& outError) {
        gui = LibGui::Get(&outError);
        if (!gui) {
            return false;
        }
        client = gui->Connect(&outError);
        if (!client) {
            return false;
        }
        token = gui->GetDisplayToken(&physicalDisplayId, &outError);
        if (!token) {
            return false;
        }
        transaction = gui->NewTransaction(&outError);
        if (!transaction && !outError.empty()) {
            return false;
        }
        return true;
    }
};

SurfaceFlingerSession* GetSession(std::string* outError = nullptr) {
    static std::mutex sInitMutex;
    static SurfaceFlingerSession* sSession = nullptr;
    static std::string sLastError;

    std::lock_guard<std::mutex> lock(sInitMutex);
    if (sSession) {
        return sSession;
    }

    auto* candidate = new SurfaceFlingerSession();
    std::string err;
    if (!candidate->Connect(err)) {
        delete candidate;
        sLastError = err;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SurfaceFlinger session init failed: %s", err.c_str());
        SetError(outError, sLastError);
        return nullptr;
    }

    sSession = candidate;
    return sSession;
}

ANativeWindow* AcquireNativeWindowFromSurface(GuiPtr surface, std::string* outError) {
    if (!surface) {
        SetError(outError, "android::Surface pointer is null");
        return nullptr;
    }

    // android::Surface derives from RefBase + ANativeWindow (second base at +2*sizeof(void*)).
    auto* window = reinterpret_cast<ANativeWindow*>(static_cast<char*>(surface) + 2 * sizeof(void*));
    uint32_t magic = 0;
    std::memcpy(&magic, window, sizeof(magic));
    if (magic != kNativeWindowMagic) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf),
                      "android::Surface layout mismatch (expected '_wnd' 0x%08x, got 0x%08x)",
                      kNativeWindowMagic, magic);
        SetError(outError, buf);
        return nullptr;
    }

    ANativeWindow_acquire(window);
    return window;
}

// Dynamic AChoreographer VSync rhythm monitor (Android 7+ via libandroid.so; zero link deps).
struct ChoreographerRhythm {
    using PrepareFn = void* (*)(int);
    using PollOnceFn = int (*)(int, int*, int*, void**);
    using GetInstanceFn = void* (*)();
    using FrameCallbackFn = void (*)(int64_t, void*);
    using PostCallbackFn = void (*)(void*, FrameCallbackFn, void*);

    static PostCallbackFn sPost;
    static std::atomic<void*> sChoreographer;
    static std::atomic<int64_t> sLastAtNs;
    static std::atomic<int64_t> sPeriodNs;
    static std::atomic<int64_t> sLastAskedNs;
    static std::atomic<bool> sAsking;

    static bool StartOnce() {
        void* androidLib = dlopen("libandroid.so", RTLD_NOW);
        if (!androidLib) {
            return false;
        }
        auto prepare = reinterpret_cast<PrepareFn>(dlsym(androidLib, "ALooper_prepare"));
        auto pollOnce = reinterpret_cast<PollOnceFn>(dlsym(androidLib, "ALooper_pollOnce"));
        auto getInst = reinterpret_cast<GetInstanceFn>(dlsym(androidLib, "AChoreographer_getInstance"));
        sPost = reinterpret_cast<PostCallbackFn>(dlsym(androidLib, "AChoreographer_postFrameCallback64"));
        if (!sPost && sizeof(long) == sizeof(int64_t)) {
            sPost = reinterpret_cast<PostCallbackFn>(dlsym(androidLib, "AChoreographer_postFrameCallback"));
        }
        if (!prepare || !pollOnce || !getInst || !sPost) {
            return false;
        }

        // ALooper cannot be stopped externally on Android; keep one lightweight daemon looper.
        std::thread([prepare, pollOnce, getInst]() {
            pthread_setname_np(pthread_self(), "esp4all-vsync");
            prepare(0);
            sChoreographer.store(getInst(), std::memory_order_release);
            for (;;) {
                pollOnce(-1, nullptr, nullptr, nullptr);
            }
        }).detach();
        return true;
    }

    static void OnFrame(int64_t frameTimeNanos, void* remainingPtr) {
        static int64_t sPrevNs = 0;
        static int64_t sShortestNs = 0;

        const intptr_t rem = reinterpret_cast<intptr_t>(remainingPtr);
        if (rem == 2) {
            sShortestNs = std::numeric_limits<int64_t>::max();
        } else if (frameTimeNanos > sPrevNs) {
            sShortestNs = std::min(sShortestNs, frameTimeNanos - sPrevNs);
        }
        sPrevNs = frameTimeNanos;

        if (rem > 0) {
            void* ch = sChoreographer.load(std::memory_order_acquire);
            if (ch && sPost) {
                sPost(ch, OnFrame, reinterpret_cast<void*>(rem - 1));
                return;
            }
        }

        // Valid display refresh intervals lie between 2 ms (500 Hz) and 100 ms (10 Hz).
        if (sShortestNs >= 2000000LL && sShortestNs <= 100000000LL) {
            sPeriodNs.store(sShortestNs, std::memory_order_relaxed);
            sLastAtNs.store(frameTimeNanos, std::memory_order_release);
        }
        sAsking.store(false, std::memory_order_release);
    }

    static bool Query(VsyncInfo& outVsync) {
        static const bool sStarted = StartOnce();
        void* ch = sChoreographer.load(std::memory_order_acquire);
        if (!sStarted || !ch || !sPost) {
            return false;
        }

        const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t lastAsked = sLastAskedNs.load(std::memory_order_relaxed);
        if ((nowNs - lastAsked) > 250000000LL && !sAsking.exchange(true, std::memory_order_acq_rel)) {
            sLastAskedNs.store(nowNs, std::memory_order_relaxed);
            sPost(ch, OnFrame, reinterpret_cast<void*>(static_cast<intptr_t>(2)));
        }

        const int64_t period = sPeriodNs.load(std::memory_order_acquire);
        if (period <= 0) {
            return false;
        }
        const int64_t atNs = sLastAtNs.load(std::memory_order_acquire);
        outVsync.timestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(atNs));
        outVsync.period = std::chrono::nanoseconds(period);
        outVsync.valid = true;
        return true;
    }
};

ChoreographerRhythm::PostCallbackFn ChoreographerRhythm::sPost = nullptr;
std::atomic<void*> ChoreographerRhythm::sChoreographer{nullptr};
std::atomic<int64_t> ChoreographerRhythm::sLastAtNs{0};
std::atomic<int64_t> ChoreographerRhythm::sPeriodNs{0};
std::atomic<int64_t> ChoreographerRhythm::sLastAskedNs{0};
std::atomic<bool> ChoreographerRhythm::sAsking{false};

} // namespace

NativeWindow::~NativeWindow() {
    Destroy();
}

NativeWindow::NativeWindow(NativeWindow&& other) noexcept
    : control_(std::exchange(other.control_, nullptr)),
      surface_(std::exchange(other.surface_, nullptr)),
      window_(std::exchange(other.window_, nullptr)),
      width_(std::exchange(other.width_, 0U)),
      height_(std::exchange(other.height_, 0U)),
      captureMode_(other.captureMode_),
      visible_(std::exchange(other.visible_, false)) {}

NativeWindow& NativeWindow::operator=(NativeWindow&& other) noexcept {
    if (this != &other) {
        Destroy();
        control_ = std::exchange(other.control_, nullptr);
        surface_ = std::exchange(other.surface_, nullptr);
        window_ = std::exchange(other.window_, nullptr);
        width_ = std::exchange(other.width_, 0U);
        height_ = std::exchange(other.height_, 0U);
        captureMode_ = other.captureMode_;
        visible_ = std::exchange(other.visible_, false);
    }
    return *this;
}

bool NativeWindow::Create(
    const char* name,
    uint32_t width,
    uint32_t height,
    CaptureMode capture,
    std::string* outError)
{
    Destroy();

    SurfaceFlingerSession* session = GetSession(outError);
    if (!session) {
        return false;
    }

    if (width == 0 || height == 0) {
        const DisplayInfo disp = QueryDisplayInfo(outError);
        if (!disp.IsValid()) {
            return false;
        }
        width = disp.width;
        height = disp.height;
    }

    GuiPtr control = session->gui->CreateSurface(
        session->client, name, width, height, capture, outError);
    if (!control) {
        return false;
    }

    GuiPtr surface = session->gui->GetSurface(control, outError);
    if (!surface) {
        session->gui->Disconnect(control);
        session->gui->Release(control);
        return false;
    }

    ANativeWindow* window = AcquireNativeWindowFromSurface(surface, outError);
    if (!window) {
        session->gui->Release(surface);
        session->gui->Disconnect(control);
        session->gui->Release(control);
        return false;
    }

    const int32_t geomStatus = ANativeWindow_setBuffersGeometry(
        window, static_cast<int32_t>(width), static_cast<int32_t>(height), WINDOW_FORMAT_RGBA_8888);
    if (geomStatus != 0) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf),
                      "ANativeWindow_setBuffersGeometry(%ux%u) failed (status %d)",
                      width, height, geomStatus);
        SetError(outError, buf);
        ANativeWindow_release(window);
        session->gui->Release(surface);
        session->gui->Disconnect(control);
        session->gui->Release(control);
        return false;
    }

    bool presented = false;
    {
        std::lock_guard<std::mutex> lock(session->txMutex);
        presented = session->gui->Present(session->transaction, control, INT32_MAX, outError);
    }
    if (!presented) {
        ANativeWindow_release(window);
        session->gui->Release(surface);
        session->gui->Disconnect(control);
        session->gui->Release(control);
        return false;
    }

    control_ = control;
    surface_ = surface;
    window_ = window;
    width_ = width;
    height_ = height;
    captureMode_ = capture;
    visible_ = true;
    return true;
}

void NativeWindow::Destroy() {
    if (!control_ && !surface_ && !window_) {
        return;
    }

    SurfaceFlingerSession* session = GetSession();
    if (session && control_) {
        std::lock_guard<std::mutex> lock(session->txMutex);
        (void)session->gui->SetVisible(session->transaction, control_, false, nullptr);
    }

    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    if (session && surface_) {
        session->gui->Release(surface_);
    }
    surface_ = nullptr;

    if (session && control_) {
        session->gui->Disconnect(control_);
        session->gui->Release(control_);
    }
    control_ = nullptr;

    width_ = 0;
    height_ = 0;
    visible_ = false;
}

bool NativeWindow::SetVisible(bool visible, std::string* outError) {
    if (!control_) {
        SetError(outError, "NativeWindow not created");
        return false;
    }
    if (visible == visible_) {
        return true;
    }

    SurfaceFlingerSession* session = GetSession(outError);
    if (!session) {
        return false;
    }

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(session->txMutex);
        ok = session->gui->SetVisible(session->transaction, control_, visible, outError);
    }
    if (ok) {
        visible_ = visible;
    }
    return ok;
}

bool NativeWindow::Resize(
    uint32_t width,
    uint32_t height,
    bool& outResizedInPlace,
    std::string* outError)
{
    outResizedInPlace = false;
    if (!control_ || !window_) {
        SetError(outError, "NativeWindow not created");
        return false;
    }
    if (width == 0 || height == 0) {
        SetError(outError, "Invalid zero resize dimensions");
        return false;
    }

    SurfaceFlingerSession* session = GetSession(outError);
    if (!session) {
        return false;
    }

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(session->txMutex);
        ok = session->gui->Resize(
            session->transaction, control_, width, height, outResizedInPlace, outError);
    }
    if (!ok || !outResizedInPlace) {
        return ok;
    }

    const int32_t geomStatus = ANativeWindow_setBuffersGeometry(
        window_, static_cast<int32_t>(width), static_cast<int32_t>(height), WINDOW_FORMAT_RGBA_8888);
    if (geomStatus != 0) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf),
                      "ANativeWindow_setBuffersGeometry(%ux%u) failed (status %d)",
                      width, height, geomStatus);
        SetError(outError, buf);
        outResizedInPlace = false;
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

DisplayInfo NativeWindow::QueryDisplayInfo(std::string* outError) {
    DisplayInfo info{};
    SurfaceFlingerSession* session = GetSession(outError);
    if (!session) {
        return info;
    }

    RawDisplayState raw{};
    if (!session->gui->GetDisplayState(session->token, raw, outError)) {
        // Retry once with a refreshed display token in case display topology changed
        GuiPtr refreshedToken = session->gui->GetDisplayToken(&session->physicalDisplayId, outError);
        if (!refreshedToken) {
            return info;
        }
        session->token = refreshedToken;
        if (!session->gui->GetDisplayState(session->token, raw, outError)) {
            return info;
        }
    }

    if (raw.width == 0 || raw.height == 0) {
        SetError(outError, "SurfaceFlinger reported empty display dimensions");
        return info;
    }

    info.width = raw.width;
    info.height = raw.height;
    info.rotation = static_cast<DisplayRotation>(raw.rotation & 3U);
    info.density = LibGui::ReadSystemDensity();
    info.refreshRate = raw.refreshRate;
    return info;
}

bool NativeWindow::QueryVsync(VsyncInfo& outVsync) {
    outVsync = {};
    return ChoreographerRhythm::Query(outVsync);
}

uint32_t NativeWindow::GetAndroidApiLevel() {
    return LibGui::ReadSystemApiLevel();
}

float NativeWindow::GetDisplayDensity() {
    return LibGui::ReadSystemDensity();
}

std::vector<std::string> NativeWindow::GetResolvedSymbols() {
    std::vector<std::string> lines;
    const LibGui* gui = LibGui::Get();
    if (!gui) {
        return lines;
    }
    const auto& picks = gui->GetPicks();
    lines.reserve(picks.size());
    for (const SymbolPick& pick : picks) {
        lines.push_back(std::string(pick.what) + ": " + pick.symbol);
    }
    return lines;
}

} // namespace esp4all
