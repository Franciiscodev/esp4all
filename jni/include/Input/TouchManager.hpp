#pragma once

#include "Input/TouchDevice.hpp"
#include "Input/TouchParser.hpp"
#include "Input/TouchRouter.hpp"
#include "Window/CaptureMode.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <imgui.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace esp4all {

/**
 * Multi-protocol Android evdev touch & pointer manager with optional /dev/uinput control mode.
 *
 * - Runs a single cleanly joinable poll()-driven worker thread (zero detached threads, 0% idle CPU).
 * - Automatically classifies Protocol B (slotted), Protocol A (frames), Single-touch, and emulator
 *   absolute pointers + wheel, honoring Android .idc configs and hotplug via inotify.
 * - Optional Control Mode (/dev/uinput): grabs the physical touchscreen only when idle, creates a
 *   virtual touchscreen for Android, forwards game touches (TouchTarget::Outside), and keeps menu
 *   touches (TouchTarget::Interface) strictly inside Dear ImGui so menu taps never click through
 *   to the game underneath. Safely rewinds axes and releases EVIOCGRAB on disable or shutdown.
 * - Includes Dear ImGui 1.92 input bridge, window confinement, and inertial touch scrolling.
 */
class TouchManager {
public:
    TouchManager() = default;
    ~TouchManager() { Stop(); }

    TouchManager(const TouchManager&) = delete;
    TouchManager& operator=(const TouchManager&) = delete;

    // Starts the input reader thread. If enableControl is true, tries to enable /dev/uinput control
    // mode immediately; falls back cleanly to Read-Only if /dev/uinput is unavailable.
    bool Start(
        int screenW,
        int screenH,
        DisplayRotation rotation,
        bool enableControl = false,
        std::string* outControlError = nullptr);

    void Stop();

    // Toggles /dev/uinput interception at runtime. On failure, stays safely in Read-Only mode.
    bool SetControl(bool enabled, std::string* outError = nullptr);
    bool IsControlEnabled() const noexcept;
    bool IsControlGrabbed() const noexcept;

    // Human-readable one-line status for DebugUI (e.g. "uinput ON (1 screen)" or "Read-Only (1 screen)").
    std::string GetStatusSummary() const;

    // Updates screen dimensions/rotation even before ImGui has rendered its first frame.
    void UpdateDisplay(int screenW, int screenH, DisplayRotation rotation);

    // Sleeps until a routed UI input event arrives or timeout elapses (used for idle power saving).
    // Returns true if input is pending.
    bool WaitForEvent(std::chrono::milliseconds timeout) const;

    // --- Render-thread ImGui 1.92 Bridge ---
    // Drains one frame of routed events and feeds them into ImGui::GetIO().
    // Returns true if any input event was delivered this frame.
    bool ApplyPendingToImGui();

    // Call immediately after ImGui::NewFrame(): confines windows inside [0..screenW, 0..screenH]
    // and drives inertial touch scrolling. Returns true while scrolling/gliding.
    bool AfterNewFrame(float screenW, float screenH);

    // Call right after ImGui::Render(): publishes visible UI window bounds to the input thread.
    void PublishInterfaceView(int screenW, int screenH, DisplayRotation rotation, float density);

private:
    struct EvdevDevice {
        std::string path;
        std::string name;
        DeviceRoles roles;
        DeviceParser parser;
        size_t slots = 0;
        bool isScreen = false;
        int fd = -1;
        std::vector<uint16_t> axes;
        bool grabbed = false;
        bool grabRefused = false;
        std::optional<AxisState> preGrabState;

        EvdevDevice(
            std::string p,
            std::string n,
            DeviceRoles r,
            const Capabilities& caps,
            size_t slotCount,
            int descriptor,
            std::vector<uint16_t> ax);
        ~EvdevDevice();

        EvdevDevice(const EvdevDevice&) = delete;
        EvdevDevice& operator=(const EvdevDevice&) = delete;
        EvdevDevice(EvdevDevice&& other) noexcept;
        EvdevDevice& operator=(EvdevDevice&&) = delete;
    };

    struct ControlState {
        int uinputFd = -1;
        VirtualFingers fingers;
        std::chrono::nanoseconds readyAt{0};
        std::vector<RawEvent> rawScratch;
        std::vector<uint8_t> byteScratch;

        void Close() noexcept;
        void Flush();
        bool Send(FingerOwner owner, TouchPhase phase, std::pair<float, float> normPos);
    };

    struct DeviceIdentity {
        uint64_t ino = 0;
        int64_t ctime = 0;
        bool operator==(const DeviceIdentity& rhs) const noexcept {
            return ino == rhs.ino && ctime == rhs.ctime;
        }
    };

    void WakeWorker() const noexcept;
    void WorkerMain();
    void ScanDevices(std::unordered_map<std::string, DeviceIdentity>& seen, uint32_t& nextDeviceId);
    void HandleDeviceReadable(uint32_t deviceId, EvdevDevice& dev, std::chrono::nanoseconds now);
    void DispatchSamples(uint32_t deviceId, EvdevDevice& dev, std::vector<DeviceSample>& samples);
    void PickScreensLocked();
    void RestoreDeviceSlotsLocked(uint32_t deviceId, EvdevDevice& dev);
    void SyncControlLocked(std::chrono::nanoseconds now);
    void ReleaseAllControlLocked();

    static std::optional<Capabilities> ReadCapabilities(int fd);
    static bool IsKeyDown(int fd, uint16_t code);
    static AxisState ReadAxisState(int fd, const std::vector<uint16_t>& axes, size_t slots);
    static IdcConfig LoadIdc(const Capabilities& device);
    static int CreateVirtualTouchscreen(std::string* outError);
    static bool GrabDevice(int fd, bool grab, std::string* outError = nullptr);
    static void RewindDevice(EvdevDevice& dev);

    // Render-thread touch scroll / ImGui bridge helpers
    void FeedEventToImGui(const InputEvent& event);
    bool DriveTouchScroll();
    static void ConfineWindows(float screenW, float screenH);

    mutable std::mutex mutex_;
    mutable std::condition_variable inputCv_;
    std::atomic<bool> running_{false};
    std::thread workerThread_;
    int wakePipe_[2]{-1, -1};
    int inotifyFd_ = -1;

    std::chrono::steady_clock::time_point startedAt_{};
    std::unordered_map<uint32_t, EvdevDevice> devices_;
    std::vector<DeviceSample> sampleScratch_;
    std::vector<InputEvent> routerScratch_;
    InterfaceView view_;
    TouchRouter router_;
    EventQueue queue_;
    ControlState control_;
    bool controlRequested_ = false;
    bool anyGrabbed_ = false;

    // Render-thread-only ImGui bridge state
    std::vector<InputEvent> frameEvents_;
    InterfaceView scratchView_;
    uint32_t activeUiFingers_ = 0;
    bool usingTouchSource_ = true;
    ImGuiID scrollLandedWindow_ = 0;
    ImGuiID scrollingWindow_ = 0;
    ImVec2 windowRestPos_{0.0f, 0.0f};
    ImVec2 scrollVelocity_{0.0f, 0.0f};
    bool scrollDragging_ = false;
};

} // namespace esp4all
