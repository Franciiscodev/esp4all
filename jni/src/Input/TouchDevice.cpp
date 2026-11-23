#include "Input/TouchDevice.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <tuple>

namespace esp4all {

namespace {

std::string_view TrimView(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

void RawEvent::Decode(
    const uint8_t* bytes,
    size_t byteLen,
    size_t timeSize,
    std::vector<RawEvent>& out)
{
    if (!bytes) {
        return;
    }
    const size_t recordSize = timeSize + sizeof(uint16_t) * 2 + sizeof(int32_t);
    for (size_t offset = 0; offset + recordSize <= byteLen; offset += recordSize) {
        const uint8_t* field = bytes + offset + timeSize;
        RawEvent event{};
        std::memcpy(&event.kind, field, sizeof(event.kind));
        std::memcpy(&event.code, field + 2, sizeof(event.code));
        std::memcpy(&event.value, field + 4, sizeof(event.value));
        out.push_back(event);
    }
}

void RawEvent::Encode(size_t timeSize, std::vector<uint8_t>& outBytes) const {
    const size_t fieldOffset = outBytes.size() + timeSize;
    outBytes.resize(fieldOffset + sizeof(kind) + sizeof(code) + sizeof(value), 0);
    std::memcpy(outBytes.data() + fieldOffset, &kind, sizeof(kind));
    std::memcpy(outBytes.data() + fieldOffset + 2, &code, sizeof(code));
    std::memcpy(outBytes.data() + fieldOffset + 4, &value, sizeof(value));
}

const std::vector<int32_t>* AxisState::FindColumn(uint16_t code) const {
    for (const auto& entry : values) {
        if (entry.first == code) {
            return &entry.second;
        }
    }
    return nullptr;
}

void AxisState::Rewind(const AxisState& earlier, std::vector<RawEvent>& out) const {
    // Approach each value from afar so the kernel jitter filter does not swallow the restore.
    constexpr uint32_t kAfar = 1U << 12;
    auto isSlotted = [](uint16_t code) {
        return code >= ev::kAbsMtFirst && code <= ev::kAbsMtLast;
    };
    auto isPositional = [&isSlotted](uint16_t code) {
        return isSlotted(code) && code != ev::kAbsMtTrackingId;
    };
    auto emitRestore = [&out](uint16_t code, int32_t value) {
        const int32_t nudge = static_cast<int32_t>(static_cast<uint32_t>(value) + kAfar);
        out.push_back(RawEvent{ev::kEvAbs, code, nudge});
        out.push_back(RawEvent{ev::kEvAbs, code, value});
    };
    auto emitSlot = [&out](int32_t slotIdx) {
        out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtSlot, slotIdx});
    };

    const size_t startCount = out.size();
    size_t maxSlots = 0;
    for (const auto& entry : values) {
        if (isPositional(entry.first)) {
            maxSlots = std::max(maxSlots, entry.second.size());
        }
    }

    for (size_t s = 0; s < maxSlots; ++s) {
        bool addressed = false;
        for (const auto& entry : values) {
            const uint16_t code = entry.first;
            const auto& held = entry.second;
            if (!isPositional(code)) {
                continue;
            }
            const auto* prev = earlier.FindColumn(code);
            if (!prev || s >= held.size() || s >= prev->size()) {
                continue;
            }
            if (held[s] != (*prev)[s]) {
                if (!addressed) {
                    emitSlot(static_cast<int32_t>(s));
                    addressed = true;
                }
                emitRestore(code, (*prev)[s]);
            }
        }
    }

    if (earlier.slot >= 0 && (out.size() > startCount || slot != earlier.slot)) {
        const size_t prevSlot = static_cast<size_t>(earlier.slot);
        for (const auto& entry : values) {
            const uint16_t code = entry.first;
            if (!isPositional(code)) {
                continue;
            }
            const auto* prev = earlier.FindColumn(code);
            if (!prev || prevSlot >= prev->size()) {
                continue;
            }
            emitSlot(earlier.slot);
            emitRestore(code, (*prev)[prevSlot]);
            break;
        }
    }

    for (const auto& entry : values) {
        const uint16_t code = entry.first;
        const auto& held = entry.second;
        if (isSlotted(code)) {
            continue;
        }
        const auto* prev = earlier.FindColumn(code);
        if (prev && held.size() == 1 && prev->size() == 1 && held[0] != (*prev)[0]) {
            emitRestore(code, (*prev)[0]);
        }
    }

    if (out.size() > startCount) {
        out.push_back(RawEvent{ev::kEvSyn, ev::kSynReport, 0});
    }
}

int32_t Axis::Span() const noexcept {
    const int64_t wide = static_cast<int64_t>(max) - static_cast<int64_t>(min);
    return static_cast<int32_t>(std::clamp<int64_t>(
        wide, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

float Axis::Normalize(int32_t value) const noexcept {
    const int64_t span = std::max<int64_t>(static_cast<int64_t>(max) - static_cast<int64_t>(min) + 1, 1);
    const float frac = static_cast<float>(static_cast<int64_t>(value) - static_cast<int64_t>(min)) /
                       static_cast<float>(span);
    return std::clamp(frac, 0.0f, 1.0f);
}

bool Capabilities::HasProperty(uint32_t property) const noexcept {
    return property < 32U && (properties & (1U << property)) != 0U;
}

bool Capabilities::HasRelative(uint16_t code) const noexcept {
    return code < 32U && (relative & (1U << code)) != 0U;
}

bool Capabilities::HasKey(uint16_t code) const noexcept {
    const size_t byteIdx = static_cast<size_t>(code) / 8U;
    return byteIdx < keys.size() && (keys[byteIdx] & (1U << (code % 8U))) != 0U;
}

bool Capabilities::HasKeyIn(uint16_t begin, uint16_t end) const noexcept {
    for (uint32_t code = begin; code < end; ++code) {
        if (HasKey(static_cast<uint16_t>(code))) {
            return true;
        }
    }
    return false;
}

bool Capabilities::HasAxis(uint16_t code) const noexcept {
    return FindAxis(code).has_value();
}

std::optional<Axis> Capabilities::FindAxis(uint16_t code) const noexcept {
    if (code >= axes.size()) {
        return std::nullopt;
    }
    const std::optional<Axis>& ax = axes[code];
    if (!ax || ax->max <= ax->min) {
        return std::nullopt;
    }
    return ax;
}

IdcConfig IdcConfig::Parse(std::string_view text) {
    IdcConfig idc;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        const size_t comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const std::string_view key = TrimView(line.substr(0, eq));
        const std::string_view val = TrimView(line.substr(eq + 1));
        if (key == "touch.deviceType") {
            idc.deviceType = std::string(val);
        } else if (key == "touch.orientationAware") {
            idc.orientationAware = (val != "0");
        } else if (key == "touch.displayId") {
            idc.displayId = std::string(val);
        } else if (key == "touch.orientation") {
            if (val == "ORIENTATION_90") {
                idc.orientation = DisplayRotation::Deg90;
            } else if (val == "ORIENTATION_180") {
                idc.orientation = DisplayRotation::Deg180;
            } else if (val == "ORIENTATION_270") {
                idc.orientation = DisplayRotation::Deg270;
            } else {
                idc.orientation = DisplayRotation::Deg0;
            }
        }
    }
    return idc;
}

std::vector<std::string> IdcConfig::FileNames(const Capabilities& device) {
    std::vector<std::string> names;
    char buf[96] = {};
    if (device.vendor != 0 && device.product != 0) {
        if (device.version != 0) {
            std::snprintf(buf, sizeof(buf), "Vendor_%04x_Product_%04x_Version_%04x.idc",
                          device.vendor, device.product, device.version);
            names.emplace_back(buf);
        }
        std::snprintf(buf, sizeof(buf), "Vendor_%04x_Product_%04x.idc",
                      device.vendor, device.product);
        names.emplace_back(buf);
    }

    std::string canonical;
    canonical.reserve(device.name.size() + 4);
    for (char c : device.name) {
        if ((static_cast<unsigned char>(c) & 0xC0U) == 0x80U) {
            continue; // UTF-8 continuation byte
        }
        const bool keep = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        canonical.push_back(keep ? c : '_');
    }
    canonical += ".idc";
    names.push_back(std::move(canonical));
    return names;
}

std::optional<std::pair<Axis, Axis>> DeviceRoles::BothAxes(
    std::optional<Axis> x,
    std::optional<Axis> y) noexcept
{
    if (!x || !y) {
        return std::nullopt;
    }
    return std::make_pair(*x, *y);
}

std::optional<ScreenConfidence> DeviceRoles::EvaluateConfidence(
    const Capabilities& device,
    const IdcConfig& idc,
    std::optional<std::string_view> displayId,
    Axis x,
    Axis y)
{
    if (idc.displayId && displayId && *idc.displayId != *displayId) {
        return std::nullopt;
    }
    if (idc.deviceType && *idc.deviceType == "touchScreen") {
        return ScreenConfidence::Certain();
    }
    if (idc.deviceType && *idc.deviceType != "default") {
        return std::nullopt;
    }
    if (device.HasProperty(ev::kPropDirect)) {
        return ScreenConfidence::Certain();
    }
    if (device.HasProperty(ev::kPropPointer)) {
        return std::nullopt;
    }

    std::string lower = device.name;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    static constexpr const char* kNotAScreen[] = {
        "fingerprint", "finger_print", "fpc", "goodix_fp", "fp_",
        "gesture", "hall", "sensor", "accel", "gyro", "proximity",
        "keyboard", "mouse", "gpio-keys", "headset", "ir_", "hbtp"
    };
    for (const char* token : kNotAScreen) {
        if (lower.find(token) != std::string::npos) {
            return std::nullopt;
        }
    }

    const int32_t span = std::min(x.Span(), y.Span());
    if (span < 200) {
        return std::nullopt;
    }
    return ScreenConfidence::Guess(span);
}

DeviceRoles DeviceRoles::Classify(
    const Capabilities& device,
    const IdcConfig& idc,
    std::optional<std::string_view> displayId)
{
    DeviceRoles roles;
    roles.wheel = device.HasRelative(ev::kRelWheel) || device.HasRelative(ev::kRelHwheel);

    const bool isGamepad = device.HasKeyIn(ev::kBtnJoystick, ev::kBtnDigi);
    const bool hasTouchBtn = device.HasKey(ev::kBtnTouch);
    const bool hasMouseBtn = device.HasKey(ev::kBtnLeft) && !isGamepad;

    const std::optional<Axis> slotAxis = device.FindAxis(ev::kAbsMtSlot);
    const bool isSlotted = slotAxis && slotAxis->min == 0 && device.HasAxis(ev::kAbsMtTrackingId);

    std::optional<std::pair<Axis, Axis>> multi = BothAxes(
        device.FindAxis(ev::kAbsMtPositionX),
        device.FindAxis(ev::kAbsMtPositionY));
    if (!hasTouchBtn && isGamepad) {
        multi.reset();
    }
    const std::optional<std::pair<Axis, Axis>> single = BothAxes(
        device.FindAxis(ev::kAbsX),
        device.FindAxis(ev::kAbsY));

    struct CandidateSurface {
        TouchProtocol protocol;
        Axis x;
        Axis y;
        uint16_t pressureAxis;
    };
    std::optional<CandidateSurface> surface;
    if (multi && isSlotted) {
        surface = CandidateSurface{TouchProtocol::Slots(), multi->first, multi->second, ev::kAbsMtPressure};
    } else if (multi) {
        surface = CandidateSurface{
            TouchProtocol::Frames(device.HasAxis(ev::kAbsMtTrackingId)),
            multi->first, multi->second, ev::kAbsMtPressure};
    } else if (single && hasTouchBtn && !hasMouseBtn) {
        surface = CandidateSurface{TouchProtocol::Single(), single->first, single->second, ev::kAbsPressure};
    }

    if (surface) {
        const auto conf = EvaluateConfidence(device, idc, displayId, surface->x, surface->y);
        if (conf) {
            TouchPanel panel{};
            panel.protocol = surface->protocol;
            panel.x = surface->x;
            panel.y = surface->y;
            panel.orientation = idc.orientation;
            panel.followsDisplay = idc.orientationAware.value_or(true);
            panel.confidence = *conf;
            panel.hasTouchButton = hasTouchBtn;
            panel.hasPressure = device.HasAxis(surface->pressureAxis);
            roles.touch = panel;
        }
    } else if (single && hasMouseBtn) {
        roles.pointer = PointerSurface{single->first, single->second};
    }

    return roles;
}

std::string DeviceRoles::Describe(bool isScreen, bool isGrabbed) const {
    std::string out;
    auto append = [&out](const std::string& part) {
        if (!out.empty()) {
            out += ", ";
        }
        out += part;
    };

    if (touch) {
        const char* proto = "Single";
        if (touch->protocol.kind == TouchProtocol::Kind::Slots) {
            proto = "Slots";
        } else if (touch->protocol.kind == TouchProtocol::Kind::Frames) {
            proto = "Frames";
        }
        char buf[96] = {};
        std::snprintf(buf, sizeof(buf), "touch %s %dx%d%s",
                      proto, touch->x.max, touch->y.max, isScreen ? " (screen)" : "");
        append(buf);
    }
    if (pointer) {
        append("pointer");
    }
    if (wheel) {
        append("wheel");
    }
    if (isGrabbed) {
        append("grabbed");
    }
    return out;
}

std::pair<float, float> TouchPanel::Rotate(float u, float v, DisplayRotation rotation) noexcept {
    switch (rotation) {
        case DisplayRotation::Deg0:   return {u, v};
        case DisplayRotation::Deg90:  return {v, 1.0f - u};
        case DisplayRotation::Deg180: return {1.0f - u, 1.0f - v};
        case DisplayRotation::Deg270: return {1.0f - v, u};
    }
    return {u, v};
}

std::array<float, 2> TouchPanel::ToScreen(
    float u,
    float v,
    std::array<float, 2> screenSize,
    DisplayRotation displayRotation) const noexcept
{
    std::tie(u, v) = Rotate(u, v, orientation);
    if (followsDisplay) {
        std::tie(u, v) = Rotate(u, v, displayRotation);
    }
    return {u * screenSize[0], v * screenSize[1]};
}

std::pair<float, float> TouchPanel::FromScreen(
    std::array<float, 2> screenPos,
    std::array<float, 2> screenSize,
    DisplayRotation displayRotation) noexcept
{
    if (screenSize[0] <= 0.0f || screenSize[1] <= 0.0f) {
        return {0.0f, 0.0f};
    }
    DisplayRotation inv = displayRotation;
    if (displayRotation == DisplayRotation::Deg90) {
        inv = DisplayRotation::Deg270;
    } else if (displayRotation == DisplayRotation::Deg270) {
        inv = DisplayRotation::Deg90;
    }
    return Rotate(screenPos[0] / screenSize[0], screenPos[1] / screenSize[1], inv);
}

} // namespace esp4all
