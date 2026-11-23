#pragma once

#include "Window/CaptureMode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esp4all {

// Linux evdev constants kept free of <linux/input.h> macro collisions so this module
// compiles identically on Android and macOS/Linux host test environments.
namespace ev {
inline constexpr uint16_t kEvSyn = 0x00;
inline constexpr uint16_t kEvKey = 0x01;
inline constexpr uint16_t kEvRel = 0x02;
inline constexpr uint16_t kEvAbs = 0x03;

inline constexpr uint16_t kSynReport = 0;
inline constexpr uint16_t kSynMtReport = 2;
inline constexpr uint16_t kSynDropped = 3;

inline constexpr uint16_t kRelHwheel = 0x06;
inline constexpr uint16_t kRelWheel = 0x08;
inline constexpr uint16_t kRelWheelHiRes = 0x0b;
inline constexpr uint16_t kRelHwheelHiRes = 0x0c;

inline constexpr uint16_t kAbsX = 0x00;
inline constexpr uint16_t kAbsY = 0x01;
inline constexpr uint16_t kAbsPressure = 0x18;
inline constexpr uint16_t kAbsMtSlot = 0x2f;
inline constexpr uint16_t kAbsMtFirst = 0x30;
inline constexpr uint16_t kAbsMtLast = 0x3d;
inline constexpr uint16_t kAbsMtPositionX = 0x35;
inline constexpr uint16_t kAbsMtPositionY = 0x36;
inline constexpr uint16_t kAbsMtToolType = 0x37;
inline constexpr uint16_t kAbsMtTrackingId = 0x39;
inline constexpr uint16_t kAbsMtPressure = 0x3a;
inline constexpr int32_t kMtToolPalm = 2;
inline constexpr size_t kAbsCount = 0x40;

inline constexpr uint16_t kBtnLeft = 0x110;
inline constexpr uint16_t kBtnRight = 0x111;
inline constexpr uint16_t kBtnMiddle = 0x112;
inline constexpr uint16_t kBtnJoystick = 0x120;
inline constexpr uint16_t kBtnDigi = 0x140;
inline constexpr uint16_t kBtnTouch = 0x14a;
inline constexpr size_t kKeyCount = 0x300;

inline constexpr uint32_t kPropPointer = 0;
inline constexpr uint32_t kPropDirect = 1;
} // namespace ev

struct RawEvent {
    uint16_t kind = 0;
    uint16_t code = 0;
    int32_t value = 0;

    bool operator==(const RawEvent& rhs) const noexcept {
        return kind == rhs.kind && code == rhs.code && value == rhs.value;
    }
    bool operator!=(const RawEvent& rhs) const noexcept { return !(*this == rhs); }

    static void Decode(const uint8_t* bytes, size_t byteLen, size_t timeSize, std::vector<RawEvent>& out);
    void Encode(size_t timeSize, std::vector<uint8_t>& outBytes) const;
};

struct AxisState {
    int32_t slot = 0;
    std::vector<std::pair<uint16_t, std::vector<int32_t>>> values;

    void Rewind(const AxisState& earlier, std::vector<RawEvent>& out) const;
    const std::vector<int32_t>* FindColumn(uint16_t code) const;
};

struct Axis {
    int32_t min = 0;
    int32_t max = 0;

    bool operator==(const Axis& rhs) const noexcept { return min == rhs.min && max == rhs.max; }
    bool operator!=(const Axis& rhs) const noexcept { return !(*this == rhs); }

    int32_t Span() const noexcept;
    float Normalize(int32_t value) const noexcept;
};

struct Capabilities {
    std::string name;
    uint16_t bus = 0;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    uint32_t properties = 0;
    uint32_t relative = 0;
    std::vector<uint8_t> keys;
    std::vector<std::optional<Axis>> axes;

    bool HasProperty(uint32_t property) const noexcept;
    bool HasRelative(uint16_t code) const noexcept;
    bool HasKey(uint16_t code) const noexcept;
    bool HasKeyIn(uint16_t begin, uint16_t end) const noexcept;
    bool HasAxis(uint16_t code) const noexcept;
    std::optional<Axis> FindAxis(uint16_t code) const noexcept;
};

struct TouchProtocol {
    enum class Kind : uint8_t { Slots, Frames, Single };

    Kind kind = Kind::Slots;
    bool tracked = false;

    static constexpr TouchProtocol Slots() noexcept { return {Kind::Slots, false}; }
    static constexpr TouchProtocol Frames(bool withIds) noexcept { return {Kind::Frames, withIds}; }
    static constexpr TouchProtocol Single() noexcept { return {Kind::Single, false}; }

    bool operator==(const TouchProtocol& rhs) const noexcept {
        return kind == rhs.kind && tracked == rhs.tracked;
    }
    bool operator!=(const TouchProtocol& rhs) const noexcept { return !(*this == rhs); }
};

struct ScreenConfidence {
    enum class Kind : uint8_t { Guess, Certain };

    Kind kind = Kind::Guess;
    int32_t span = 0;

    static constexpr ScreenConfidence Guess(int32_t shorterSpan) noexcept {
        return {Kind::Guess, shorterSpan};
    }
    static constexpr ScreenConfidence Certain() noexcept {
        return {Kind::Certain, 0};
    }

    bool operator==(const ScreenConfidence& rhs) const noexcept {
        return kind == rhs.kind && span == rhs.span;
    }
    bool operator<(const ScreenConfidence& rhs) const noexcept {
        if (kind != rhs.kind) {
            return static_cast<uint8_t>(kind) < static_cast<uint8_t>(rhs.kind);
        }
        return span < rhs.span;
    }
};

struct TouchPanel {
    TouchProtocol protocol;
    Axis x;
    Axis y;
    DisplayRotation orientation = DisplayRotation::Deg0;
    bool followsDisplay = true;
    ScreenConfidence confidence;
    bool hasTouchButton = false;
    bool hasPressure = false;

    static std::pair<float, float> Rotate(float u, float v, DisplayRotation rotation) noexcept;
    std::array<float, 2> ToScreen(
        float u,
        float v,
        std::array<float, 2> screenSize,
        DisplayRotation displayRotation) const noexcept;
    static std::pair<float, float> FromScreen(
        std::array<float, 2> screenPos,
        std::array<float, 2> screenSize,
        DisplayRotation displayRotation) noexcept;
};

struct PointerSurface {
    Axis x;
    Axis y;

    static constexpr std::array<float, 2> ToScreen(
        float u,
        float v,
        std::array<float, 2> screenSize) noexcept
    {
        return {u * screenSize[0], v * screenSize[1]};
    }
};

struct IdcConfig {
    std::optional<std::string> deviceType;
    DisplayRotation orientation = DisplayRotation::Deg0;
    std::optional<bool> orientationAware;
    std::optional<std::string> displayId;

    static IdcConfig Parse(std::string_view text);
    static std::vector<std::string> FileNames(const Capabilities& device);
};

struct DeviceRoles {
    std::optional<TouchPanel> touch;
    std::optional<PointerSurface> pointer;
    bool wheel = false;

    bool IsEmpty() const noexcept {
        return !touch && !pointer && !wheel;
    }

    static DeviceRoles Classify(
        const Capabilities& device,
        const IdcConfig& idc,
        std::optional<std::string_view> displayId = std::nullopt);

    std::string Describe(bool isScreen, bool isGrabbed) const;

private:
    static std::optional<std::pair<Axis, Axis>> BothAxes(
        std::optional<Axis> x,
        std::optional<Axis> y) noexcept;
    static std::optional<ScreenConfidence> EvaluateConfidence(
        const Capabilities& device,
        const IdcConfig& idc,
        std::optional<std::string_view> displayId,
        Axis x,
        Axis y);
};

} // namespace esp4all
