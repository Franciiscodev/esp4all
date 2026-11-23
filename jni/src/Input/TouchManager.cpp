#include "Input/TouchManager.hpp"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <imgui_internal.h>
#include <poll.h>
#include <pthread.h>
#include <sstream>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace esp4all {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

constexpr size_t kTimevalSize = sizeof(timeval);
constexpr size_t kUinputNameSize = 80;
constexpr uint16_t kBusVirtual = 0x06;
constexpr const char* kDeviceDirs[] = {"/dev/input", "/dev"};

// Raw Linux ioctl helpers (avoids <linux/input.h> macro collisions).
constexpr unsigned long IoctlRead(uint32_t nr, size_t size) {
    return static_cast<unsigned long>(_IOC(_IOC_READ, 'E', nr, size));
}

bool QueryEvdev(int fd, uint32_t nr, void* dst, size_t size) {
    return ::ioctl(fd, IoctlRead(nr, size), dst) >= 0;
}

template <typename T>
bool QueryEvdevObj(int fd, uint32_t nr, T& dst) {
    return QueryEvdev(fd, nr, &dst, sizeof(dst));
}

bool IoctlUinput(int fd, unsigned long req, unsigned long val, std::string* outError = nullptr) {
    if (::ioctl(fd, req, val) < 0) {
        if (outError) {
            *outError = std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool ReadTextFile(const std::string& path, std::string& outText) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    outText = ss.str();
    return true;
}

int ButtonToImGuiIndex(MouseButton btn) noexcept {
    switch (btn) {
        case MouseButton::Right:  return 1;
        case MouseButton::Middle: return 2;
        case MouseButton::Left:
        default:                  return 0;
    }
}

bool CanWindowScroll(const ImGuiWindow& win, int axis) noexcept {
    const bool locked = (win.Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0;
    return !locked && (axis == 0 ? win.ScrollMax.x : win.ScrollMax.y) > 0.0f;
}

ImGuiWindow* FindScrollableWindow(ImGuiWindow* win, int axis) noexcept {
    for (; win; win = (win->Flags & ImGuiWindowFlags_ChildWindow) ? win->ParentWindow : nullptr) {
        if (CanWindowScroll(*win, axis)) {
            return win;
        }
    }
    return nullptr;
}

bool IsFreeToScroll(const ImGuiContext& ctx, ImGuiWindow& win, ImVec2 landedPos) {
    if (ctx.ActiveId == 0) {
        return true;
    }
    ImGuiWindow* root = win.RootWindow;
    if (ctx.ActiveId == root->MoveId || ctx.ActiveId == win.MoveId) {
        return !root->TitleBarRect().Contains(landedPos);
    }
    if (ctx.ActiveIdUsingNavDirMask != 0) {
        return false;
    }
    for (ImGuiWindow* owner = ctx.ActiveIdWindow;
         owner;
         owner = (owner->Flags & ImGuiWindowFlags_ChildWindow) ? owner->ParentWindow : nullptr) {
        if (ctx.ActiveId == ImGui::GetWindowScrollbarID(owner, ImGuiAxis_X) ||
            ctx.ActiveId == ImGui::GetWindowScrollbarID(owner, ImGuiAxis_Y)) {
            return false;
        }
        for (int corner = 0; corner < 4; ++corner) {
            if (ctx.ActiveId == ImGui::GetWindowResizeCornerID(owner, corner)) {
                return false;
            }
        }
        for (int border = 0; border < 4; ++border) {
            if (ctx.ActiveId == ImGui::GetWindowResizeBorderID(owner, static_cast<ImGuiDir>(border))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TouchManager::EvdevDevice::EvdevDevice(
    std::string p,
    std::string n,
    DeviceRoles r,
    const Capabilities& caps,
    size_t slotCount,
    int descriptor,
    std::vector<uint16_t> ax)
    : path(std::move(p)),
      name(std::move(n)),
      roles(std::move(r)),
      parser(caps, roles),
      slots(slotCount),
      fd(descriptor),
      axes(std::move(ax)) {}

TouchManager::EvdevDevice::~EvdevDevice() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

TouchManager::EvdevDevice::EvdevDevice(EvdevDevice&& other) noexcept
    : path(std::move(other.path)),
      name(std::move(other.name)),
      roles(std::move(other.roles)),
      parser(std::move(other.parser)),
      slots(other.slots),
      isScreen(other.isScreen),
      fd(std::exchange(other.fd, -1)),
      axes(std::move(other.axes)),
      grabbed(other.grabbed),
      grabRefused(other.grabRefused),
      preGrabState(std::move(other.preGrabState)) {}

void TouchManager::ControlState::Close() noexcept {
    if (uinputFd >= 0) {
        ::ioctl(uinputFd, static_cast<unsigned long>(_IO('U', 2)), 0UL);
        ::close(uinputFd);
        uinputFd = -1;
    }
    fingers = VirtualFingers{};
    readyAt = nanoseconds::zero();
    rawScratch.clear();
    byteScratch.clear();
}

void TouchManager::ControlState::Flush() {
    if (uinputFd < 0 || rawScratch.empty()) {
        rawScratch.clear();
        return;
    }
    byteScratch.clear();
    for (const RawEvent& e : rawScratch) {
        e.Encode(kTimevalSize, byteScratch);
    }
    rawScratch.clear();
    if (!byteScratch.empty()) {
        const ssize_t wrote = ::write(uinputFd, byteScratch.data(), byteScratch.size());
        if (wrote != static_cast<ssize_t>(byteScratch.size())) {
            std::printf("[TouchManager] virtual touchscreen write failed: %s\n", std::strerror(errno));
        }
    }
}

bool TouchManager::ControlState::Send(
    FingerOwner owner,
    TouchPhase phase,
    std::pair<float, float> normPos)
{
    if (uinputFd < 0) {
        return false;
    }
    const bool changed = fingers.Apply(owner, phase, normPos, rawScratch);
    Flush();
    return changed;
}

std::optional<Capabilities> TouchManager::ReadCapabilities(int fd) {
    char nameBuf[128] = {};
    uint16_t idBuf[4] = {};
    uint8_t propBuf[4] = {};
    uint8_t relBuf[4] = {};
    uint8_t absBits[ev::kAbsCount / 8] = {};

    Capabilities caps;
    caps.keys.assign(ev::kKeyCount / 8, 0);
    caps.axes.assign(ev::kAbsCount, std::nullopt);

    if (!QueryEvdevObj(fd, 0x02, idBuf)) {
        return std::nullopt;
    }
    QueryEvdev(fd, 0x06, nameBuf, sizeof(nameBuf) - 1);
    QueryEvdevObj(fd, 0x09, propBuf);
    QueryEvdevObj(fd, 0x20U + ev::kEvRel, relBuf);
    QueryEvdevObj(fd, 0x20U + ev::kEvAbs, absBits);
    QueryEvdev(fd, 0x20U + ev::kEvKey, caps.keys.data(), caps.keys.size());

    caps.name = nameBuf;
    caps.bus = idBuf[0];
    caps.vendor = idBuf[1];
    caps.product = idBuf[2];
    caps.version = idBuf[3];
    std::memcpy(&caps.properties, propBuf, sizeof(caps.properties));
    std::memcpy(&caps.relative, relBuf, sizeof(caps.relative));

    for (size_t code = 0; code < ev::kAbsCount; ++code) {
        if ((absBits[code / 8] & (1U << (code % 8))) == 0U) {
            continue;
        }
        int32_t absInfo[6] = {};
        if (QueryEvdevObj(fd, 0x40U + static_cast<uint32_t>(code), absInfo)) {
            caps.axes[code] = Axis{absInfo[1], absInfo[2]};
        }
    }
    return caps;
}

bool TouchManager::IsKeyDown(int fd, uint16_t code) {
    uint8_t keys[ev::kKeyCount / 8] = {};
    if (!QueryEvdevObj(fd, 0x18, keys)) {
        return false;
    }
    const size_t idx = static_cast<size_t>(code) / 8U;
    return idx < sizeof(keys) && (keys[idx] & (1U << (code % 8U))) != 0U;
}

AxisState TouchManager::ReadAxisState(int fd, const std::vector<uint16_t>& axes, size_t slots) {
    AxisState state;
    for (uint16_t code : axes) {
        if (code >= ev::kAbsMtFirst && code <= ev::kAbsMtLast) {
            if (slots > 0) {
                std::vector<int32_t> vals(slots + 1, 0);
                vals[0] = static_cast<int32_t>(code);
                if (QueryEvdev(fd, 0x0a, vals.data(), vals.size() * sizeof(int32_t))) {
                    vals.erase(vals.begin());
                    state.values.emplace_back(code, std::move(vals));
                }
            }
            continue;
        }
        int32_t info[6] = {};
        if (!QueryEvdevObj(fd, 0x40U + static_cast<uint32_t>(code), info)) {
            continue;
        }
        if (code == ev::kAbsMtSlot) {
            state.slot = info[0];
        } else {
            state.values.emplace_back(code, std::vector<int32_t>{info[0]});
        }
    }
    return state;
}

IdcConfig TouchManager::LoadIdc(const Capabilities& device) {
    static constexpr const char* kIdcDirs[] = {
        "/product/usr/idc",
        "/system_ext/usr/idc",
        "/odm/usr/idc",
        "/vendor/usr/idc",
        "/system/usr/idc",
        "/data/system/devices/idc"
    };
    const std::vector<std::string> names = IdcConfig::FileNames(device);
    std::string text;
    for (const char* dir : kIdcDirs) {
        for (const std::string& name : names) {
            if (ReadTextFile(std::string(dir) + "/" + name, text)) {
                return IdcConfig::Parse(text);
            }
        }
    }
    return IdcConfig{};
}

int TouchManager::CreateVirtualTouchscreen(std::string* outError) {
    static constexpr const char* kUinputPaths[] = {
        "/dev/uinput", "/dev/input/uinput", "/dev/misc/uinput"
    };
    int fd = -1;
    int lastErr = ENOENT;
    for (const char* path : kUinputPaths) {
        fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            break;
        }
        lastErr = errno;
    }
    if (fd < 0) {
        if (outError) {
            *outError = std::string("Cannot open /dev/uinput: ") + std::strerror(lastErr);
        }
        return -1;
    }

    const unsigned long kDevCreate = static_cast<unsigned long>(_IO('U', 1));
    const unsigned long kSetEvBit = static_cast<unsigned long>(_IOW('U', 100, int));
    const unsigned long kSetKeyBit = static_cast<unsigned long>(_IOW('U', 101, int));
    const unsigned long kSetAbsBit = static_cast<unsigned long>(_IOW('U', 103, int));
    const unsigned long kSetPropBit = static_cast<unsigned long>(_IOW('U', 110, int));

    const std::pair<uint16_t, int32_t> kAxes[] = {
        {ev::kAbsMtSlot, static_cast<int32_t>(kVirtualSlots) - 1},
        {ev::kAbsMtTrackingId, 0xFFFF},
        {ev::kAbsMtPositionX, kVirtualAxisRange - 1},
        {ev::kAbsMtPositionY, kVirtualAxisRange - 1}
    };

    bool ok = IoctlUinput(fd, kSetEvBit, ev::kEvSyn, outError) &&
              IoctlUinput(fd, kSetEvBit, ev::kEvKey, outError) &&
              IoctlUinput(fd, kSetEvBit, ev::kEvAbs, outError) &&
              IoctlUinput(fd, kSetKeyBit, ev::kBtnTouch, outError) &&
              IoctlUinput(fd, kSetPropBit, ev::kPropDirect, outError);
    for (const auto& ax : kAxes) {
        ok = ok && IoctlUinput(fd, kSetAbsBit, ax.first, outError);
    }

    if (ok) {
        // struct uinput_user_dev: char name[80], struct input_id (8B), uint32 ff_effects_max (4B),
        // int32 absmax[64], absmin[64], absfuzz[64], absflat[64].
        std::vector<uint8_t> setup(kUinputNameSize + 12 + 4 * 4 * ev::kAbsCount, 0);
        const size_t copyLen = std::min(kVirtualTouchscreenName.size(), kUinputNameSize - 1);
        std::memcpy(setup.data(), kVirtualTouchscreenName.data(), copyLen);
        std::memcpy(setup.data() + kUinputNameSize, &kBusVirtual, sizeof(kBusVirtual));
        for (const auto& ax : kAxes) {
            std::memcpy(setup.data() + kUinputNameSize + 12 + 4 * static_cast<size_t>(ax.first),
                        &ax.second, sizeof(ax.second));
        }

        if (::write(fd, setup.data(), setup.size()) != static_cast<ssize_t>(setup.size())) {
            if (outError) {
                *outError = std::string("uinput setup write failed: ") + std::strerror(errno);
            }
            ok = false;
        } else {
            ok = IoctlUinput(fd, kDevCreate, 0, outError);
        }
    }

    if (!ok) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool TouchManager::GrabDevice(int fd, bool grab, std::string* outError) {
    const unsigned long req = static_cast<unsigned long>(_IOW('E', 0x90, int));
    if (::ioctl(fd, req, static_cast<unsigned long>(grab ? 1 : 0)) < 0) {
        if (outError) {
            *outError = std::strerror(errno);
        }
        return false;
    }
    return true;
}

void TouchManager::RewindDevice(EvdevDevice& dev) {
    if (!dev.preGrabState || dev.fd < 0) {
        dev.preGrabState.reset();
        return;
    }
    const AxisState earlier = std::move(*dev.preGrabState);
    dev.preGrabState.reset();

    std::vector<RawEvent> raw;
    std::vector<uint8_t> bytes;
    ReadAxisState(dev.fd, dev.axes, dev.slots).Rewind(earlier, raw);
    if (raw.empty()) {
        return;
    }
    for (const RawEvent& e : raw) {
        e.Encode(kTimevalSize, bytes);
    }
    const int wfd = ::open(dev.path.c_str(), O_WRONLY | O_CLOEXEC);
    if (wfd >= 0) {
        (void)::write(wfd, bytes.data(), bytes.size());
        ::close(wfd);
    }
}

void TouchManager::WakeWorker() const noexcept {
    if (wakePipe_[1] >= 0) {
        const uint8_t byte = 1;
        (void)::write(wakePipe_[1], &byte, 1);
    }
}

bool TouchManager::Start(
    int screenW,
    int screenH,
    DisplayRotation rotation,
    bool enableControl,
    std::string* outControlError)
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    if (::pipe2(wakePipe_, O_NONBLOCK | O_CLOEXEC) != 0) {
        std::printf("[TouchManager] pipe2 failed: %s\n", std::strerror(errno));
        return false;
    }

    inotifyFd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ >= 0) {
        int watched = 0;
        for (const char* dir : kDeviceDirs) {
            if (::inotify_add_watch(inotifyFd_, dir, IN_CREATE | IN_DELETE | IN_MOVED_TO) >= 0) {
                ++watched;
            }
        }
        if (watched == 0) {
            ::close(inotifyFd_);
            inotifyFd_ = -1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        startedAt_ = Clock::now();
        view_.size = {static_cast<float>(screenW), static_cast<float>(screenH)};
        view_.rotation = rotation;
        controlRequested_ = false;
        anyGrabbed_ = false;
    }

    running_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&TouchManager::WorkerMain, this);

    if (enableControl) {
        if (!SetControl(true, outControlError)) {
            std::printf("[TouchManager] Control mode unavailable (%s), continuing in Read-Only mode\n",
                        (outControlError && !outControlError->empty()) ? outControlError->c_str() : "uinput error");
        }
    }

    return true;
}

void TouchManager::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    WakeWorker();
    inputCv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    if (inotifyFd_ >= 0) {
        ::close(inotifyFd_);
        inotifyFd_ = -1;
    }
    if (wakePipe_[0] >= 0) {
        ::close(wakePipe_[0]);
        wakePipe_[0] = -1;
    }
    if (wakePipe_[1] >= 0) {
        ::close(wakePipe_[1]);
        wakePipe_[1] = -1;
    }
}

bool TouchManager::SetControl(bool enabled, std::string* outError) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const nanoseconds now = Clock::now() - startedAt_;
        if (enabled && control_.uinputFd < 0) {
            const int ufd = CreateVirtualTouchscreen(outError);
            if (ufd < 0) {
                controlRequested_ = false;
                return false;
            }
            control_.uinputFd = ufd;
            // Give Android EventHub 500ms to register the virtual touchscreen before grabbing hardware.
            control_.readyAt = now + milliseconds(500);
        }
        controlRequested_ = enabled;
        SyncControlLocked(now);
    }
    WakeWorker();
    return true;
}

bool TouchManager::IsControlEnabled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return controlRequested_;
}

bool TouchManager::IsControlGrabbed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return anyGrabbed_;
}

std::string TouchManager::GetStatusSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t screens = 0;
    size_t pointers = 0;
    for (const auto& entry : devices_) {
        if (entry.second.isScreen) {
            ++screens;
        }
        if (entry.second.roles.pointer) {
            ++pointers;
        }
    }
    char buf[96] = {};
    const char* mode = controlRequested_
        ? (anyGrabbed_ ? "uinput ON" : "uinput waiting")
        : "Read-Only";
    if (pointers > 0) {
        std::snprintf(buf, sizeof(buf), "%s (%zu scr, %zu ptr)", mode, screens, pointers);
    } else {
        std::snprintf(buf, sizeof(buf), "%s (%zu scr)", mode, screens);
    }
    return buf;
}

void TouchManager::UpdateDisplay(int screenW, int screenH, DisplayRotation rotation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (screenW > 0 && screenH > 0) {
        view_.size = {static_cast<float>(screenW), static_cast<float>(screenH)};
        view_.rotation = rotation;
    }
}

bool TouchManager::WaitForEvent(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!queue_.IsEmpty()) {
        return true;
    }
    inputCv_.wait_for(lock, timeout, [this]() {
        return !queue_.IsEmpty() || !running_.load(std::memory_order_acquire);
    });
    return !queue_.IsEmpty();
}

void TouchManager::PickScreensLocked() {
    std::optional<ScreenConfidence> best;
    for (const auto& entry : devices_) {
        const auto& dev = entry.second;
        if (dev.roles.touch && (!best || *best < dev.roles.touch->confidence)) {
            best = dev.roles.touch->confidence;
        }
    }
    for (auto& entry : devices_) {
        auto& dev = entry.second;
        dev.isScreen = dev.roles.touch.has_value() &&
                       (dev.roles.touch->confidence == ScreenConfidence::Certain() ||
                        (best && dev.roles.touch->confidence == *best));
    }
}

void TouchManager::RestoreDeviceSlotsLocked(uint32_t deviceId, EvdevDevice& dev) {
    if (dev.slots == 0 || !dev.roles.touch ||
        dev.roles.touch->protocol != TouchProtocol::Slots()) {
        return;
    }
    TouchParser* tp = dev.parser.TouchMut();
    if (!tp) {
        return;
    }
    std::vector<TouchContact> contacts;
    tp->RestoreSlots(ReadAxisState(dev.fd, dev.axes, dev.slots), IsKeyDown(dev.fd, ev::kBtnTouch), contacts);
    if (contacts.empty()) {
        return;
    }
    for (const TouchContact& c : contacts) {
        sampleScratch_.push_back(DeviceSample{c});
    }
    DispatchSamples(deviceId, dev, sampleScratch_);
}

void TouchManager::SyncControlLocked(nanoseconds now) {
    if (control_.uinputFd < 0) {
        anyGrabbed_ = false;
        return;
    }

    const bool ready = controlRequested_ && (now >= control_.readyAt);
    bool anyGrab = false;

    for (auto& entry : devices_) {
        const uint32_t devId = entry.first;
        EvdevDevice& dev = entry.second;
        const bool wantGrab = ready && dev.isScreen && !dev.grabRefused;
        const bool settled = dev.parser.IsIdle() && !control_.fingers.HoldsDevice(devId);

        if (dev.grabbed != wantGrab && settled) {
            if (!wantGrab) {
                RewindDevice(dev);
                (void)GrabDevice(dev.fd, false);
                dev.grabbed = false;
            } else {
                std::string grabErr;
                if (!GrabDevice(dev.fd, true, &grabErr)) {
                    std::printf("[TouchManager] EVIOCGRAB refused on %s: %s\n",
                                dev.path.c_str(), grabErr.c_str());
                    dev.grabRefused = true;
                } else {
                    AxisState st = ReadAxisState(dev.fd, dev.axes, dev.slots);
                    if (dev.parser.IsTouching(st, IsKeyDown(dev.fd, ev::kBtnTouch))) {
                        // Finger landed right before grab; yield immediately so Android does not get stuck.
                        (void)GrabDevice(dev.fd, false);
                    } else {
                        dev.grabbed = true;
                        dev.preGrabState = std::move(st);
                    }
                }
            }
        }
        anyGrab = anyGrab || dev.grabbed;
    }

    anyGrabbed_ = anyGrab;
    if (!controlRequested_ && !anyGrab) {
        control_.fingers.LiftAll(control_.rawScratch);
        control_.Flush();
        control_.Close();
    }
}

void TouchManager::ReleaseAllControlLocked() {
    controlRequested_ = false;
    for (auto& entry : devices_) {
        EvdevDevice& dev = entry.second;
        if (dev.grabbed) {
            RewindDevice(dev);
            (void)GrabDevice(dev.fd, false);
            dev.grabbed = false;
        }
    }
    anyGrabbed_ = false;
    if (control_.uinputFd >= 0) {
        control_.fingers.LiftAll(control_.rawScratch);
        control_.Flush();
        control_.Close();
    }
}

void TouchManager::DispatchSamples(
    uint32_t deviceId,
    EvdevDevice& dev,
    std::vector<DeviceSample>& samples)
{
    routerScratch_.clear();
    for (const DeviceSample& sample : samples) {
        if (const auto* contact = std::get_if<TouchContact>(&sample.value)) {
            if (!dev.roles.touch || !dev.isScreen) {
                continue;
            }
            const auto pos = dev.roles.touch->ToScreen(
                contact->u, contact->v, view_.size, view_.rotation);
            const TouchTarget target = router_.RouteContact(
                deviceId, *contact, pos, view_, routerScratch_);
            const bool forwardToGame =
                (target == TouchTarget::Outside) &&
                (dev.grabbed || contact->phase != TouchPhase::Start);
            if (control_.uinputFd >= 0 && forwardToGame) {
                control_.Send(
                    FingerOwner{deviceId, contact->id},
                    contact->phase,
                    TouchPanel::FromScreen(pos, view_.size, view_.rotation));
            }
        } else if (const auto* hover = std::get_if<DeviceSample::Hover>(&sample.value)) {
            router_.RouteHover(
                PointerSurface::ToScreen(hover->u, hover->v, view_.size), view_, routerScratch_);
        } else if (const auto* btn = std::get_if<DeviceSample::Button>(&sample.value)) {
            router_.RouteButton(btn->button, btn->pressed, view_, routerScratch_);
        } else if (const auto* wheel = std::get_if<DeviceSample::Wheel>(&sample.value)) {
            router_.RouteWheel(wheel->x, wheel->y, view_, routerScratch_);
        }
    }
    samples.clear();

    const Clock::time_point now = Clock::now();
    const bool pushedAny = !routerScratch_.empty();
    for (InputEvent& ev : routerScratch_) {
        queue_.Push(std::move(ev), now);
    }
    routerScratch_.clear();
    if (pushedAny) {
        inputCv_.notify_one();
    }
}

void TouchManager::ScanDevices(
    std::unordered_map<std::string, DeviceIdentity>& seen,
    uint32_t& nextDeviceId)
{
    for (const char* dirPath : kDeviceDirs) {
        DIR* dir = ::opendir(dirPath);
        if (!dir) {
            continue;
        }
        while (const dirent* entry = ::readdir(dir)) {
            const char* name = entry->d_name;
            if (std::strncmp(name, "event", 5) != 0) {
                continue;
            }
            const std::string fullPath = std::string(dirPath) + "/" + name;
            struct stat st{};
            // Major 13 = Linux input event devices (prevents reading pipes/sockets named event*).
            if (::stat(fullPath.c_str(), &st) != 0 ||
                !S_ISCHR(st.st_mode) ||
                ((st.st_rdev >> 8) & 0xFFFU) != 13U) {
                continue;
            }
            const DeviceIdentity id{static_cast<uint64_t>(st.st_ino), static_cast<int64_t>(st.st_ctime)};
            auto it = seen.find(fullPath);
            if (it != seen.end() && it->second == id) {
                continue;
            }
            seen[fullPath] = id;

            const int fd = ::open(fullPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            const auto caps = ReadCapabilities(fd);
            if (!caps || caps->name == kVirtualTouchscreenName) {
                ::close(fd);
                continue;
            }
            const DeviceRoles roles = DeviceRoles::Classify(*caps, LoadIdc(*caps));
            if (roles.IsEmpty()) {
                ::close(fd);
                continue;
            }

            const auto slotAxis = caps->FindAxis(ev::kAbsMtSlot);
            const size_t slots = slotAxis
                ? std::min<size_t>(static_cast<size_t>(std::max(slotAxis->max, -1) + 1), 32U)
                : 0U;
            std::vector<uint16_t> axes;
            for (size_t code = 0; code < caps->axes.size(); ++code) {
                if (caps->axes[code]) {
                    axes.push_back(static_cast<uint16_t>(code));
                }
            }

            const uint32_t devId = ++nextDeviceId;
            EvdevDevice dev(fullPath, caps->name, roles, *caps, slots, fd, std::move(axes));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto inserted = devices_.emplace(devId, std::move(dev));
                PickScreensLocked();
                std::printf("[TouchManager] Opened %s \"%s\" [%s]\n",
                            fullPath.c_str(),
                            inserted.first->second.name.c_str(),
                            inserted.first->second.roles.Describe(
                                inserted.first->second.isScreen, false).c_str());
                RestoreDeviceSlotsLocked(devId, inserted.first->second);
                SyncControlLocked(Clock::now() - startedAt_);
            }
        }
        ::closedir(dir);
    }
}

void TouchManager::HandleDeviceReadable(uint32_t deviceId, EvdevDevice& dev, nanoseconds now) {
    uint8_t buf[(kTimevalSize + 8) * 64];
    std::vector<RawEvent> raw;
    bool dropped = false;

    for (;;) {
        const ssize_t n = ::read(dev.fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            // Device removed/broken; mark fd invalid so WorkerMain cleans it up.
            ::close(dev.fd);
            dev.fd = -1;
            break;
        }
        if (n == 0) {
            ::close(dev.fd);
            dev.fd = -1;
            break;
        }

        raw.clear();
        RawEvent::Decode(buf, static_cast<size_t>(n), kTimevalSize, raw);
        bool needResync = false;
        for (const RawEvent& e : raw) {
            if (e.kind == ev::kEvSyn && e.code == ev::kSynDropped) {
                dropped = true;
                dev.parser.Cancel(sampleScratch_);
            } else if (!dropped) {
                dev.parser.Feed(e, now, sampleScratch_);
            } else if (e.kind == ev::kEvSyn && e.code == ev::kSynReport) {
                dropped = false;
                needResync = true;
            }
        }

        DispatchSamples(deviceId, dev, sampleScratch_);
        if (needResync) {
            RestoreDeviceSlotsLocked(deviceId, dev);
        }
    }
}

void TouchManager::WorkerMain() {
    pthread_setname_np(pthread_self(), "esp4all-input");

    std::unordered_map<std::string, DeviceIdentity> seen;
    uint32_t nextDeviceId = 0;
    ScanDevices(seen, nextDeviceId);

    std::optional<nanoseconds> pendingRescanAt;
    if (inotifyFd_ < 0) {
        pendingRescanAt = (Clock::now() - startedAt_) + std::chrono::seconds(1);
    }

    std::vector<pollfd> pfds;
    std::vector<uint32_t> pollDevIds;

    while (running_.load(std::memory_order_acquire)) {
        const nanoseconds now = Clock::now() - startedAt_;
        std::optional<nanoseconds> nextDeadline = pendingRescanAt;

        pfds.clear();
        pollDevIds.clear();
        pfds.push_back(pollfd{wakePipe_[0], POLLIN, 0});
        if (inotifyFd_ >= 0) {
            pfds.push_back(pollfd{inotifyFd_, POLLIN, 0});
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& entry : devices_) {
                if (entry.second.fd >= 0) {
                    pfds.push_back(pollfd{entry.second.fd, POLLIN, 0});
                    pollDevIds.push_back(entry.first);
                }
                const auto dl = entry.second.parser.Deadline();
                if (dl && (!nextDeadline || *dl < *nextDeadline)) {
                    nextDeadline = dl;
                }
            }
            if (control_.uinputFd >= 0 && controlRequested_ && now < control_.readyAt) {
                if (!nextDeadline || control_.readyAt < *nextDeadline) {
                    nextDeadline = control_.readyAt;
                }
            }
        }

        int timeoutMs = -1;
        if (nextDeadline) {
            const nanoseconds rem = *nextDeadline - now;
            if (rem <= nanoseconds::zero()) {
                timeoutMs = 0;
            } else {
                const int64_t ms = std::chrono::duration_cast<milliseconds>(rem).count();
                timeoutMs = static_cast<int>(std::clamp<int64_t>(ms + 1, 1, 1000));
            }
        }

        const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (ready < 0 && errno == EINTR) {
            continue;
        }

        // Drain wake pipe
        if (ready > 0 && (pfds[0].revents & POLLIN) != 0) {
            uint8_t drain[64];
            while (::read(wakePipe_[0], drain, sizeof(drain)) > 0) {
            }
        }

        // Drain inotify hotplug notifications -> schedule rescan after 200ms settle
        size_t devPollStart = 1;
        if (inotifyFd_ >= 0) {
            devPollStart = 2;
            if (ready > 0 && (pfds[1].revents & POLLIN) != 0) {
                char evBuf[2048];
                while (::read(inotifyFd_, evBuf, sizeof(evBuf)) > 0) {
                }
                pendingRescanAt = (Clock::now() - startedAt_) + milliseconds(200);
            }
        }

        const nanoseconds afterNow = Clock::now() - startedAt_;
        bool needRescanNow = false;
        if (pendingRescanAt && afterNow >= *pendingRescanAt) {
            needRescanNow = true;
            pendingRescanAt = (inotifyFd_ < 0)
                ? std::optional<nanoseconds>(afterNow + std::chrono::seconds(1))
                : std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<uint32_t> removedIds;

            if (ready > 0) {
                for (size_t i = 0; i < pollDevIds.size(); ++i) {
                    const short rev = pfds[devPollStart + i].revents;
                    if (rev == 0) {
                        continue;
                    }
                    const uint32_t devId = pollDevIds[i];
                    auto it = devices_.find(devId);
                    if (it == devices_.end()) {
                        continue;
                    }
                    if ((rev & POLLIN) != 0) {
                        HandleDeviceReadable(devId, it->second, afterNow);
                    }
                    if ((rev & (POLLHUP | POLLERR | POLLNVAL)) != 0 || it->second.fd < 0) {
                        removedIds.push_back(devId);
                    }
                }
            }

            // Tick protocol deadlines (e.g. FramesParser untracked release timeout)
            for (auto& entry : devices_) {
                const auto dl = entry.second.parser.Deadline();
                if (dl && afterNow >= *dl) {
                    entry.second.parser.Tick(afterNow, sampleScratch_);
                    DispatchSamples(entry.first, entry.second, sampleScratch_);
                }
            }

            for (uint32_t remId : removedIds) {
                auto it = devices_.find(remId);
                if (it != devices_.end()) {
                    seen.erase(it->second.path);
                    it->second.parser.Cancel(sampleScratch_);
                    DispatchSamples(remId, it->second, sampleScratch_);
                    if (control_.uinputFd >= 0) {
                        control_.fingers.LiftDevice(remId, control_.rawScratch);
                        control_.Flush();
                    }
                    devices_.erase(it);
                }
            }
            if (!removedIds.empty()) {
                PickScreensLocked();
            }

            SyncControlLocked(afterNow);
        }

        if (needRescanNow) {
            ScanDevices(seen, nextDeviceId);
        }
    }

    // Guaranteed clean teardown: rewind axes, release EVIOCGRAB, lift virtual fingers, close all FDs.
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseAllControlLocked();
    devices_.clear();
}

void TouchManager::FeedEventToImGui(const InputEvent& event) {
    ImGuiIO& io = ImGui::GetIO();
    auto emitSource = [&]() {
        io.AddMouseSourceEvent(usingTouchSource_ ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
    };

    if (const auto* t = std::get_if<InputEvent::Touch>(&event.value)) {
        if (t->phase == TouchPhase::Start) {
            ++activeUiFingers_;
            usingTouchSource_ = true;
        } else if (t->phase != TouchPhase::Move && activeUiFingers_ > 0) {
            --activeUiFingers_;
        }
    } else if (const auto* moved = std::get_if<InputEvent::PointerMoved>(&event.value)) {
        emitSource();
        io.AddMousePosEvent(moved->position[0], moved->position[1]);
    } else if (const auto* btn = std::get_if<InputEvent::PointerButton>(&event.value)) {
        emitSource();
        io.AddMousePosEvent(btn->position[0], btn->position[1]);
        io.AddMouseButtonEvent(ButtonToImGuiIndex(btn->button), btn->pressed);
    } else if (std::holds_alternative<InputEvent::PointerGone>(event.value)) {
        emitSource();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        if (usingTouchSource_) {
            io.AddMouseButtonEvent(0, false);
        }
        usingTouchSource_ = (activeUiFingers_ > 0);
    } else if (const auto* wheel = std::get_if<InputEvent::Wheel>(&event.value)) {
        io.AddMouseWheelEvent(-wheel->x, wheel->y);
    }
}

bool TouchManager::ApplyPendingToImGui() {
    frameEvents_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)queue_.TakeFrame(frameEvents_);
    }
    for (const InputEvent& ev : frameEvents_) {
        FeedEventToImGui(ev);
    }
    return !frameEvents_.empty();
}

void TouchManager::ConfineWindows(float screenW, float screenH) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || screenW <= 0.0f || screenH <= 0.0f) {
        return;
    }
    constexpr ImGuiWindowFlags kIgnored =
        ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip |
        ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoMove;

    auto clampAxis = [](float pos, float size, float room) -> float {
        return (size >= room) ? 0.0f : std::clamp(pos, 0.0f, room - size);
    };

    for (int i = 0; i < ctx->Windows.Size; ++i) {
        ImGuiWindow* win = ctx->Windows[i];
        if (!win || !win->WasActive || (win->Flags & kIgnored) != 0) {
            continue;
        }
        const ImVec2 kept(clampAxis(win->Pos.x, win->Size.x, screenW),
                          clampAxis(win->Pos.y, win->Size.y, screenH));
        if (kept.x != win->Pos.x || kept.y != win->Pos.y) {
            ImGui::SetWindowPos(win, kept, ImGuiCond_Always);
        }
    }
}

bool TouchManager::DriveTouchScroll() {
    constexpr float kSlowestGlide = 30.0f;
    constexpr float kSmoothing = 0.5f;

    ImGuiContext* ctxPtr = ImGui::GetCurrentContext();
    if (!ctxPtr) {
        return false;
    }
    ImGuiContext& ctx = *ctxPtr;
    const ImGuiIO& io = ctx.IO;
    const bool isFinger = (io.MouseSource == ImGuiMouseSource_TouchScreen);

    if (io.MouseClicked[0]) {
        scrollLandedWindow_ = (isFinger && ctx.HoveredWindow) ? ctx.HoveredWindow->ID : 0;
        if (scrollLandedWindow_ != 0) {
            windowRestPos_ = ctx.HoveredWindow->RootWindow->Pos;
        }
        scrollingWindow_ = 0;
        scrollDragging_ = false;
        scrollVelocity_ = ImVec2(0.0f, 0.0f);
    }

    if (io.MouseDown[0] && isFinger && scrollLandedWindow_ != 0) {
        const ImVec2 moved(io.MousePos.x - io.MouseClickedPos[0].x,
                           io.MousePos.y - io.MouseClickedPos[0].y);
        if (!scrollDragging_ && std::hypot(moved.x, moved.y) > io.MouseDragThreshold) {
            ImGuiWindow* under = ImGui::FindWindowByID(scrollLandedWindow_);
            const int axis = (std::fabs(moved.x) > std::fabs(moved.y)) ? 0 : 1;
            ImGuiWindow* target = under ? FindScrollableWindow(under, axis) : nullptr;
            if (target && IsFreeToScroll(ctx, *under, io.MouseClickedPos[0])) {
                if (ctx.MovingWindow) {
                    ImGui::SetWindowPos(ctx.MovingWindow->RootWindow, windowRestPos_);
                    ctx.MovingWindow = nullptr;
                }
                ImGui::SetActiveID(target->GetID("##esp4all-touch-scroll"), target);
                scrollingWindow_ = target->ID;
                scrollDragging_ = true;
            } else {
                scrollLandedWindow_ = 0;
            }
        }
        if (scrollDragging_) {
            ImGuiWindow* target = ImGui::FindWindowByID(scrollingWindow_);
            if (!target) {
                scrollDragging_ = false;
                return false;
            }
            ImGui::KeepAliveID(target->GetID("##esp4all-touch-scroll"));
            if (CanWindowScroll(*target, 0)) {
                ImGui::SetScrollX(target, target->Scroll.x - io.MouseDelta.x);
            }
            if (CanWindowScroll(*target, 1)) {
                ImGui::SetScrollY(target, target->Scroll.y - io.MouseDelta.y);
            }
            const float invDt = 1.0f / std::max(io.DeltaTime, 1e-4f);
            scrollVelocity_.x += (io.MouseDelta.x * invDt - scrollVelocity_.x) * kSmoothing;
            scrollVelocity_.y += (io.MouseDelta.y * invDt - scrollVelocity_.y) * kSmoothing;
            return true;
        }
        return false;
    }

    if (scrollDragging_) {
        scrollDragging_ = false;
        ImGuiWindow* target = ImGui::FindWindowByID(scrollingWindow_);
        if (target && ctx.ActiveId == target->GetID("##esp4all-touch-scroll")) {
            ImGui::ClearActiveID();
        }
    }
    if (scrollingWindow_ == 0) {
        return false;
    }

    ImGuiWindow* target = ImGui::FindWindowByID(scrollingWindow_);
    const bool horiz = target && CanWindowScroll(*target, 0) && std::fabs(scrollVelocity_.x) > kSlowestGlide;
    const bool vert = target && CanWindowScroll(*target, 1) && std::fabs(scrollVelocity_.y) > kSlowestGlide;
    if (!horiz && !vert) {
        scrollingWindow_ = 0;
        return false;
    }

    const float dt = std::min(io.DeltaTime, 0.1f);
    const ImVec2 before = target->Scroll;
    if (horiz) {
        ImGui::SetScrollX(target, std::clamp(target->Scroll.x - scrollVelocity_.x * dt, 0.0f, target->ScrollMax.x));
    }
    if (vert) {
        ImGui::SetScrollY(target, std::clamp(target->Scroll.y - scrollVelocity_.y * dt, 0.0f, target->ScrollMax.y));
    }
    const float decay = std::exp(-4.0f * dt);
    scrollVelocity_.x *= decay;
    scrollVelocity_.y *= decay;

    const ImVec2 wanted = target->ScrollTarget;
    const bool hitEnd = (!horiz || wanted.x == before.x) && (!vert || wanted.y == before.y);
    if (hitEnd) {
        scrollingWindow_ = 0;
    }
    return !hitEnd;
}

bool TouchManager::AfterNewFrame(float screenW, float screenH) {
    ConfineWindows(screenW, screenH);
    return DriveTouchScroll();
}

void TouchManager::PublishInterfaceView(
    int screenW,
    int screenH,
    DisplayRotation rotation,
    float density)
{
    scratchView_.size = {static_cast<float>(screenW), static_cast<float>(screenH)};
    scratchView_.rotation = rotation;
    scratchView_.regions.clear();
    scratchView_.dismissable = false;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->FrameCountEnded > 0) {
        const float margin = 8.0f * std::clamp(density, 1.0f, 5.0f);
        for (int i = 0; i < ctx->Windows.Size; ++i) {
            const ImGuiWindow* win = ctx->Windows[i];
            if (!win || !win->Active || win->Hidden) {
                continue;
            }
            if ((win->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoMouseInputs)) != 0) {
                continue;
            }
            const ImRect r = win->Rect();
            scratchView_.regions.push_back(UiRect{
                r.Min.x - margin,
                r.Min.y - margin,
                r.Max.x + margin,
                r.Max.y + margin
            });
        }
        scratchView_.dismissable = (ctx->OpenPopupStack.Size > 0);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(view_, scratchView_);
}

} // namespace esp4all
