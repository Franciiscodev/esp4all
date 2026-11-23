#pragma once

#include "Input/TouchDevice.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace esp4all {

enum class TouchPhase : uint8_t { Start, Move, End, Cancel };

struct TouchContact {
    uint32_t id = 0;
    TouchPhase phase = TouchPhase::Start;
    float u = 0.0f;
    float v = 0.0f;
};

class TouchParser {
public:
    static std::unique_ptr<TouchParser> Create(const TouchPanel& setup);
    virtual ~TouchParser() = default;

    virtual std::optional<std::chrono::nanoseconds> Deadline() const { return std::nullopt; }
    virtual void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<TouchContact>& out) = 0;
    virtual void Tick(std::chrono::nanoseconds /*now*/, std::vector<TouchContact>& /*out*/) {}
    virtual bool IsIdle() const = 0;
    virtual void Cancel(std::vector<TouchContact>& out) = 0;
    virtual void RestoreSlots(const AxisState& /*state*/, bool /*pressed*/, std::vector<TouchContact>& /*out*/) {}

    bool IsTouching(const AxisState& state, bool pressed) const;

protected:
    struct Point {
        int32_t x = 0;
        int32_t y = 0;
        std::optional<int32_t> tracking;
    };

    struct LiveContact {
        uint32_t id = 0;
        Point point;
    };

    static constexpr size_t kMaxContacts = 32;

    explicit TouchParser(const TouchPanel& setup) : setup_(setup) {}

    bool Hovers(bool pressed, int32_t pressure, bool palm) const noexcept;
    TouchContact MakeContact(uint32_t id, TouchPhase phase, const Point& point) const noexcept;
    uint32_t FreshId() noexcept;
    static int32_t ValueAt(const std::vector<int32_t>* column, size_t slot) noexcept;

    TouchPanel setup_;
    bool pressed_ = false;

private:
    uint32_t nextId_ = 0;
};

// Multitouch Protocol B (slotted — modern Android phones & tablets).
class SlotsParser final : public TouchParser {
public:
    explicit SlotsParser(const TouchPanel& setup) : TouchParser(setup) {}

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<TouchContact>& out) override;
    bool IsIdle() const override;
    void Cancel(std::vector<TouchContact>& out) override;
    void RestoreSlots(const AxisState& state, bool pressed, std::vector<TouchContact>& out) override;

private:
    struct Held {
        uint32_t id = 0;
        int32_t tracking = 0;
    };

    struct Slot {
        int32_t tracking = -1;
        int32_t x = 0;
        int32_t y = 0;
        int32_t pressure = 0;
        bool palm = false;
        std::optional<Held> live;
        bool moved = false;
    };

    Slot& CurrentSlot();
    void Flush(std::vector<TouchContact>& out);

    std::vector<Slot> slots_;
    size_t current_ = 0;
};

// Multitouch Protocol A (whole-frame reports — emulators & older panels).
class FramesParser final : public TouchParser {
public:
    explicit FramesParser(const TouchPanel& setup) : TouchParser(setup) {}

    std::optional<std::chrono::nanoseconds> Deadline() const override { return releaseAt_; }
    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<TouchContact>& out) override;
    void Tick(std::chrono::nanoseconds now, std::vector<TouchContact>& out) override;
    bool IsIdle() const override { return live_.empty(); }
    void Cancel(std::vector<TouchContact>& out) override;

private:
    struct Partial {
        std::optional<int32_t> x;
        std::optional<int32_t> y;
        std::optional<int32_t> tracking;
        std::optional<int32_t> pressure;
    };

    void PushPartial();
    void Commit(std::chrono::nanoseconds now, std::vector<TouchContact>& out);
    void Finish(TouchPhase phase, std::vector<TouchContact>& out);

    Partial partial_;
    Point last_;
    std::vector<Point> frame_;
    std::vector<LiveContact> live_;
    std::optional<std::chrono::nanoseconds> heldSince_;
    std::optional<std::chrono::nanoseconds> releaseAt_;
    bool fading_ = false;
};

// Single-touch panel (ABS_X / ABS_Y + BTN_TOUCH).
class SingleParser final : public TouchParser {
public:
    explicit SingleParser(const TouchPanel& setup) : TouchParser(setup) {}

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<TouchContact>& out) override;
    bool IsIdle() const override { return !live_.has_value(); }
    void Cancel(std::vector<TouchContact>& out) override;

private:
    void Finish(TouchPhase phase, std::vector<TouchContact>& out);

    std::optional<int32_t> x_;
    std::optional<int32_t> y_;
    Point last_;
    std::optional<LiveContact> live_;
    int32_t pressure_ = 0;
};

inline constexpr std::string_view kVirtualTouchscreenName = "esp4all virtual touch";
inline constexpr size_t kVirtualSlots = 16;
inline constexpr int32_t kVirtualAxisRange = 32768;

struct FingerOwner {
    uint32_t device = 0;
    uint32_t contact = 0;

    bool operator==(const FingerOwner& rhs) const noexcept {
        return device == rhs.device && contact == rhs.contact;
    }
};

// Virtual touchscreen slot state + Protocol B RawEvent synthesizer for /dev/uinput.
class VirtualFingers {
public:
    bool IsEmpty() const noexcept;
    bool HoldsDevice(uint32_t deviceId) const noexcept;
    bool Apply(FingerOwner owner, TouchPhase phase, std::pair<float, float> normPos, std::vector<RawEvent>& out);
    void LiftAll(std::vector<RawEvent>& out);
    void LiftDevice(uint32_t deviceId, std::vector<RawEvent>& out);

private:
    std::array<std::optional<FingerOwner>, kVirtualSlots> owners_;
    int32_t nextTrackingId_ = 0;
};

enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2 };

struct DeviceSample {
    struct Hover {
        float u = 0.0f;
        float v = 0.0f;
    };
    struct Button {
        MouseButton button = MouseButton::Left;
        bool pressed = false;
    };
    struct Wheel {
        float x = 0.0f;
        float y = 0.0f;
    };

    std::variant<TouchContact, Hover, Button, Wheel> value;
};

class DeviceParser {
public:
    DeviceParser(const Capabilities& device, const DeviceRoles& roles);

    TouchParser* TouchMut() noexcept { return touch_.get(); }
    std::optional<std::chrono::nanoseconds> Deadline() const;
    bool IsIdle() const;
    bool IsTouching(const AxisState& state, bool pressed) const;

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<DeviceSample>& out);
    void Tick(std::chrono::nanoseconds now, std::vector<DeviceSample>& out);
    void Cancel(std::vector<DeviceSample>& out);

private:
    void EmitContacts(std::vector<DeviceSample>& out);
    void FlushNonTouch(std::vector<DeviceSample>& out);

    std::unique_ptr<TouchParser> touch_;
    std::optional<PointerSurface> pointer_;
    bool wheel_ = false;
    bool preciseWheel_ = false;
    std::array<int32_t, 2> pointerPos_{0, 0};
    bool pointerMoved_ = false;
    std::vector<DeviceSample::Button> pendingButtons_;
    std::array<float, 2> pendingScroll_{0.0f, 0.0f};
    std::vector<TouchContact> scratchContacts_;
};

} // namespace esp4all
