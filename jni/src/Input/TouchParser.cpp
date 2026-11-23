#include "Input/TouchParser.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace esp4all {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;

bool VirtualFingers::IsEmpty() const noexcept {
    for (const auto& owner : owners_) {
        if (owner.has_value()) {
            return false;
        }
    }
    return true;
}

bool VirtualFingers::HoldsDevice(uint32_t deviceId) const noexcept {
    for (const auto& owner : owners_) {
        if (owner && owner->device == deviceId) {
            return true;
        }
    }
    return false;
}

bool VirtualFingers::Apply(
    FingerOwner owner,
    TouchPhase phase,
    std::pair<float, float> normPos,
    std::vector<RawEvent>& out)
{
    std::optional<size_t> heldSlot;
    std::optional<size_t> freeSlot;
    for (size_t i = 0; i < owners_.size(); ++i) {
        if (owners_[i] && *owners_[i] == owner) {
            heldSlot = i;
            break;
        }
        if (!owners_[i] && !freeSlot) {
            freeSlot = i;
        }
    }

    const bool lifting = (phase == TouchPhase::End || phase == TouchPhase::Cancel);
    std::optional<size_t> slot = heldSlot;
    if (!slot && phase == TouchPhase::Start) {
        slot = freeSlot;
    }
    if (!slot) {
        return false;
    }

    out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtSlot, static_cast<int32_t>(*slot)});
    if (lifting) {
        owners_[*slot].reset();
        out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtTrackingId, -1});
        if (IsEmpty()) {
            out.push_back(RawEvent{ev::kEvKey, ev::kBtnTouch, 0});
        }
    } else {
        if (!heldSlot) {
            if (IsEmpty()) {
                out.push_back(RawEvent{ev::kEvKey, ev::kBtnTouch, 1});
            }
            owners_[*slot] = owner;
            nextTrackingId_ = (nextTrackingId_ + 1) & 0xFFFF;
            out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtTrackingId, nextTrackingId_});
        }

        auto toUnit = [](float frac) -> int32_t {
            const float scaled = frac * static_cast<float>(kVirtualAxisRange);
            if (!(scaled > 0.0f)) {
                return 0;
            }
            if (scaled >= static_cast<float>(kVirtualAxisRange - 1)) {
                return kVirtualAxisRange - 1;
            }
            return static_cast<int32_t>(scaled);
        };
        out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtPositionX, toUnit(normPos.first)});
        out.push_back(RawEvent{ev::kEvAbs, ev::kAbsMtPositionY, toUnit(normPos.second)});
    }

    out.push_back(RawEvent{ev::kEvSyn, ev::kSynReport, 0});
    return true;
}

void VirtualFingers::LiftAll(std::vector<RawEvent>& out) {
    const auto snapshot = owners_;
    for (const auto& owner : snapshot) {
        if (owner) {
            Apply(*owner, TouchPhase::End, {0.0f, 0.0f}, out);
        }
    }
}

void VirtualFingers::LiftDevice(uint32_t deviceId, std::vector<RawEvent>& out) {
    const auto snapshot = owners_;
    for (const auto& owner : snapshot) {
        if (owner && owner->device == deviceId) {
            Apply(*owner, TouchPhase::End, {0.0f, 0.0f}, out);
        }
    }
}

std::unique_ptr<TouchParser> TouchParser::Create(const TouchPanel& setup) {
    switch (setup.protocol.kind) {
        case TouchProtocol::Kind::Slots:
            return std::make_unique<SlotsParser>(setup);
        case TouchProtocol::Kind::Frames:
            return std::make_unique<FramesParser>(setup);
        case TouchProtocol::Kind::Single:
        default:
            return std::make_unique<SingleParser>(setup);
    }
}

bool TouchParser::Hovers(bool pressed, int32_t pressure, bool palm) const noexcept {
    return (setup_.hasTouchButton && !pressed) || (setup_.hasPressure && pressure <= 0) || palm;
}

TouchContact TouchParser::MakeContact(uint32_t id, TouchPhase phase, const Point& point) const noexcept {
    return TouchContact{id, phase, setup_.x.Normalize(point.x), setup_.y.Normalize(point.y)};
}

uint32_t TouchParser::FreshId() noexcept {
    return ++nextId_;
}

int32_t TouchParser::ValueAt(const std::vector<int32_t>* column, size_t slot) noexcept {
    return (column && slot < column->size()) ? (*column)[slot] : 0;
}

bool TouchParser::IsTouching(const AxisState& state, bool pressed) const {
    const auto* tracking = state.FindColumn(ev::kAbsMtTrackingId);
    if (!tracking) {
        return false;
    }
    const auto* pressure = state.FindColumn(ev::kAbsMtPressure);
    const auto* tool = state.FindColumn(ev::kAbsMtToolType);
    for (size_t s = 0; s < tracking->size(); ++s) {
        if ((*tracking)[s] >= 0 &&
            !Hovers(pressed, ValueAt(pressure, s), ValueAt(tool, s) == ev::kMtToolPalm)) {
            return true;
        }
    }
    return false;
}

void SlotsParser::Feed(RawEvent event, nanoseconds /*now*/, std::vector<TouchContact>& out) {
    if (event.kind == ev::kEvAbs) {
        switch (event.code) {
            case ev::kAbsMtSlot:
                current_ = static_cast<size_t>(
                    std::clamp(event.value, 0, static_cast<int32_t>(kMaxContacts) - 1));
                break;
            case ev::kAbsMtTrackingId:
                CurrentSlot().tracking = event.value;
                break;
            case ev::kAbsMtPressure:
                CurrentSlot().pressure = event.value;
                break;
            case ev::kAbsMtToolType:
                CurrentSlot().palm = (event.value == ev::kMtToolPalm);
                break;
            case ev::kAbsMtPositionX: {
                Slot& slot = CurrentSlot();
                slot.x = event.value;
                slot.moved = true;
                break;
            }
            case ev::kAbsMtPositionY: {
                Slot& slot = CurrentSlot();
                slot.y = event.value;
                slot.moved = true;
                break;
            }
            default:
                break;
        }
    } else if (event.kind == ev::kEvKey && event.code == ev::kBtnTouch) {
        pressed_ = (event.value != 0);
    } else if (event.kind == ev::kEvSyn && event.code == ev::kSynReport) {
        Flush(out);
    }
}

bool SlotsParser::IsIdle() const {
    for (const Slot& slot : slots_) {
        if (slot.live.has_value()) {
            return false;
        }
    }
    return true;
}

void SlotsParser::Cancel(std::vector<TouchContact>& out) {
    for (Slot& slot : slots_) {
        if (slot.live) {
            const Held held = *slot.live;
            slot.live.reset();
            out.push_back(MakeContact(held.id, TouchPhase::Cancel, Point{slot.x, slot.y, std::nullopt}));
        }
    }
    slots_.clear();
    pressed_ = false;
}

void SlotsParser::RestoreSlots(const AxisState& state, bool pressed, std::vector<TouchContact>& out) {
    const auto* tracking = state.FindColumn(ev::kAbsMtTrackingId);
    const auto* xs = state.FindColumn(ev::kAbsMtPositionX);
    const auto* ys = state.FindColumn(ev::kAbsMtPositionY);
    const auto* pressures = state.FindColumn(ev::kAbsMtPressure);
    const auto* tools = state.FindColumn(ev::kAbsMtToolType);

    slots_.clear();
    if (tracking) {
        const size_t count = std::min(tracking->size(), kMaxContacts);
        slots_.reserve(count);
        for (size_t s = 0; s < count; ++s) {
            Slot restored{};
            restored.tracking = (*tracking)[s];
            restored.x = ValueAt(xs, s);
            restored.y = ValueAt(ys, s);
            restored.pressure = ValueAt(pressures, s);
            restored.palm = (ValueAt(tools, s) == ev::kMtToolPalm);
            slots_.push_back(restored);
        }
    }
    current_ = (state.slot < 0)
        ? 0U
        : std::min(static_cast<size_t>(state.slot), kMaxContacts - 1);
    pressed_ = pressed;
    Flush(out);
}

SlotsParser::Slot& SlotsParser::CurrentSlot() {
    if (slots_.size() <= current_) {
        slots_.resize(current_ + 1);
    }
    return slots_[current_];
}

void SlotsParser::Flush(std::vector<TouchContact>& out) {
    for (Slot& slot : slots_) {
        const Point pt{slot.x, slot.y, std::nullopt};
        const bool active = (slot.tracking >= 0) && !Hovers(pressed_, slot.pressure, slot.palm);
        if (slot.live && active && slot.live->tracking == slot.tracking) {
            if (slot.moved) {
                out.push_back(MakeContact(slot.live->id, TouchPhase::Move, pt));
            }
        } else {
            if (slot.live) {
                out.push_back(MakeContact(slot.live->id, TouchPhase::End, pt));
            }
            if (active) {
                slot.live = Held{FreshId(), slot.tracking};
                out.push_back(MakeContact(slot.live->id, TouchPhase::Start, pt));
            } else {
                slot.live.reset();
            }
        }
        slot.moved = false;
    }
}

void FramesParser::Feed(RawEvent event, nanoseconds now, std::vector<TouchContact>& out) {
    if (event.kind == ev::kEvAbs) {
        switch (event.code) {
            case ev::kAbsMtPositionX:
                partial_.x = event.value;
                break;
            case ev::kAbsMtPositionY:
                partial_.y = event.value;
                break;
            case ev::kAbsMtTrackingId:
                partial_.tracking = event.value;
                break;
            case ev::kAbsMtPressure:
                partial_.pressure = event.value;
                break;
            default:
                break;
        }
    } else if (event.kind == ev::kEvKey && event.code == ev::kBtnTouch) {
        pressed_ = (event.value != 0);
    } else if (event.kind == ev::kEvSyn && event.code == ev::kSynMtReport) {
        PushPartial();
    } else if (event.kind == ev::kEvSyn && event.code == ev::kSynReport) {
        PushPartial();
        Tick(now, out);
        Commit(now, out);
    }
}

void FramesParser::Tick(nanoseconds now, std::vector<TouchContact>& out) {
    if (releaseAt_ && now >= *releaseAt_) {
        Finish(TouchPhase::End, out);
    }
}

void FramesParser::Cancel(std::vector<TouchContact>& out) {
    Finish(TouchPhase::Cancel, out);
    frame_.clear();
    partial_ = {};
    pressed_ = false;
}

void FramesParser::Finish(TouchPhase phase, std::vector<TouchContact>& out) {
    for (const LiveContact& live : live_) {
        out.push_back(MakeContact(live.id, phase, live.point));
    }
    live_.clear();
    heldSince_.reset();
    releaseAt_.reset();
    fading_ = false;
}

void FramesParser::PushPartial() {
    const Partial p = std::exchange(partial_, Partial{});
    if (!p.x && !p.y) {
        return;
    }
    last_ = Point{p.x.value_or(last_.x), p.y.value_or(last_.y), p.tracking};
    const bool hovering = setup_.hasPressure && p.pressure && *p.pressure <= 0;
    if (!hovering && frame_.size() < kMaxContacts) {
        frame_.push_back(last_);
    }
}

void FramesParser::Commit(nanoseconds now, std::vector<TouchContact>& out) {
    const bool tracked = setup_.protocol.tracked;
    const bool hasBtn = setup_.hasTouchButton;
    const bool unreliable = !tracked && !hasBtn;

    if (frame_.empty() || (hasBtn && !pressed_)) {
        if (!unreliable || live_.empty()) {
            Finish(TouchPhase::End, out);
        } else if (!std::exchange(fading_, true)) {
            const bool longHold = heldSince_ &&
                                  std::max(now - *heldSince_, nanoseconds::zero()) >= milliseconds(200);
            releaseAt_ = now + (longHold ? milliseconds(250) : milliseconds(40));
        }
        frame_.clear();
        return;
    }

    const std::vector<Point>& points = frame_;
    const std::vector<LiveContact> previous = std::exchange(live_, {});
    std::array<bool, kMaxContacts> taken{};
    std::array<std::optional<size_t>, kMaxContacts> owner{};

    if (tracked) {
        for (size_t pi = 0; pi < points.size(); ++pi) {
            for (size_t li = 0; li < previous.size(); ++li) {
                if (previous[li].point.tracking == points[pi].tracking) {
                    owner[pi] = li;
                    break;
                }
            }
        }
    } else {
        const size_t rounds = std::min(previous.size(), points.size());
        for (size_t round = 0; round < rounds; ++round) {
            struct Match {
                uint64_t distSq;
                size_t liveIdx;
                size_t ptIdx;
            };
            std::optional<Match> best;
            for (size_t li = 0; li < previous.size(); ++li) {
                if (taken[li]) {
                    continue;
                }
                for (size_t pi = 0; pi < points.size(); ++pi) {
                    if (owner[pi]) {
                        continue;
                    }
                    const int64_t dx = static_cast<int64_t>(points[pi].x) -
                                       static_cast<int64_t>(previous[li].point.x);
                    const int64_t dy = static_cast<int64_t>(points[pi].y) -
                                       static_cast<int64_t>(previous[li].point.y);
                    const uint64_t distSq = static_cast<uint64_t>(dx * dx) +
                                            static_cast<uint64_t>(dy * dy);
                    if (!best || distSq < best->distSq) {
                        best = Match{distSq, li, pi};
                    }
                }
            }
            if (!best) {
                break;
            }
            taken[best->liveIdx] = true;
            owner[best->ptIdx] = best->liveIdx;
        }
    }

    for (size_t pi = 0; pi < points.size(); ++pi) {
        if (owner[pi]) {
            taken[*owner[pi]] = true;
        }
    }
    for (size_t li = 0; li < previous.size(); ++li) {
        if (!taken[li]) {
            out.push_back(MakeContact(previous[li].id, TouchPhase::End, previous[li].point));
        }
    }
    for (size_t pi = 0; pi < points.size(); ++pi) {
        const Point& pt = points[pi];
        uint32_t id = 0;
        if (owner[pi]) {
            const LiveContact& prev = previous[*owner[pi]];
            if (prev.point.x != pt.x || prev.point.y != pt.y) {
                out.push_back(MakeContact(prev.id, TouchPhase::Move, pt));
            }
            id = prev.id;
        } else {
            id = FreshId();
            out.push_back(MakeContact(id, TouchPhase::Start, pt));
        }
        live_.push_back(LiveContact{id, pt});
    }

    fading_ = false;
    if (previous.empty() || !heldSince_) {
        heldSince_ = now;
    }
    releaseAt_ = unreliable ? std::optional<nanoseconds>(now + seconds(1)) : std::nullopt;
    frame_.clear();
}

void SingleParser::Feed(RawEvent event, nanoseconds /*now*/, std::vector<TouchContact>& out) {
    if (event.kind == ev::kEvAbs) {
        switch (event.code) {
            case ev::kAbsX:
                x_ = event.value;
                break;
            case ev::kAbsY:
                y_ = event.value;
                break;
            case ev::kAbsPressure:
                pressure_ = event.value;
                break;
            default:
                break;
        }
    } else if (event.kind == ev::kEvKey && event.code == ev::kBtnTouch) {
        pressed_ = (event.value != 0);
    } else if (event.kind == ev::kEvSyn && event.code == ev::kSynReport) {
        const std::optional<int32_t> nx = std::exchange(x_, std::nullopt);
        const std::optional<int32_t> ny = std::exchange(y_, std::nullopt);
        const bool moved = nx.has_value() || ny.has_value();
        last_ = Point{nx.value_or(last_.x), ny.value_or(last_.y), std::nullopt};
        const bool touchingNow = pressed_ && !(setup_.hasPressure && pressure_ <= 0);
        if (!live_ && touchingNow) {
            live_ = LiveContact{FreshId(), last_};
            out.push_back(MakeContact(live_->id, TouchPhase::Start, last_));
        } else if (live_ && touchingNow && moved) {
            live_->point = last_;
            out.push_back(MakeContact(live_->id, TouchPhase::Move, last_));
        } else if (live_ && !touchingNow) {
            Finish(TouchPhase::End, out);
        }
    }
}

void SingleParser::Cancel(std::vector<TouchContact>& out) {
    Finish(TouchPhase::Cancel, out);
    x_.reset();
    y_.reset();
    pressed_ = false;
}

void SingleParser::Finish(TouchPhase phase, std::vector<TouchContact>& out) {
    if (live_) {
        const LiveContact l = *live_;
        live_.reset();
        out.push_back(MakeContact(l.id, phase, l.point));
    }
}

DeviceParser::DeviceParser(const Capabilities& device, const DeviceRoles& roles)
    : pointer_(roles.pointer),
      wheel_(roles.wheel),
      preciseWheel_(device.HasRelative(ev::kRelWheelHiRes) || device.HasRelative(ev::kRelHwheelHiRes))
{
    if (roles.touch) {
        touch_ = TouchParser::Create(*roles.touch);
    }
}

std::optional<nanoseconds> DeviceParser::Deadline() const {
    return touch_ ? touch_->Deadline() : std::nullopt;
}

bool DeviceParser::IsIdle() const {
    return !touch_ || touch_->IsIdle();
}

bool DeviceParser::IsTouching(const AxisState& state, bool pressed) const {
    return touch_ && touch_->IsTouching(state, pressed);
}

void DeviceParser::Feed(RawEvent event, nanoseconds now, std::vector<DeviceSample>& out) {
    if (touch_) {
        touch_->Feed(event, now, scratchContacts_);
        EmitContacts(out);
    }

    constexpr float kWheelDetent = 120.0f;
    if (event.kind == ev::kEvAbs) {
        if (pointer_ && event.code == ev::kAbsX) {
            pointerPos_[0] = event.value;
            pointerMoved_ = true;
        } else if (pointer_ && event.code == ev::kAbsY) {
            pointerPos_[1] = event.value;
            pointerMoved_ = true;
        }
    } else if (event.kind == ev::kEvKey && pointer_) {
        if (event.code == ev::kBtnLeft || event.code == ev::kBtnTouch) {
            pendingButtons_.push_back(DeviceSample::Button{MouseButton::Left, event.value != 0});
        } else if (event.code == ev::kBtnRight) {
            pendingButtons_.push_back(DeviceSample::Button{MouseButton::Right, event.value != 0});
        } else if (event.code == ev::kBtnMiddle) {
            pendingButtons_.push_back(DeviceSample::Button{MouseButton::Middle, event.value != 0});
        }
    } else if (event.kind == ev::kEvRel && wheel_) {
        if (!preciseWheel_ && event.code == ev::kRelWheel) {
            pendingScroll_[1] += static_cast<float>(event.value);
        } else if (!preciseWheel_ && event.code == ev::kRelHwheel) {
            pendingScroll_[0] += static_cast<float>(event.value);
        } else if (event.code == ev::kRelWheelHiRes) {
            pendingScroll_[1] += static_cast<float>(event.value) / kWheelDetent;
        } else if (event.code == ev::kRelHwheelHiRes) {
            pendingScroll_[0] += static_cast<float>(event.value) / kWheelDetent;
        }
    } else if (event.kind == ev::kEvSyn && event.code == ev::kSynReport) {
        FlushNonTouch(out);
    }
}

void DeviceParser::Tick(nanoseconds now, std::vector<DeviceSample>& out) {
    if (touch_) {
        touch_->Tick(now, scratchContacts_);
        EmitContacts(out);
    }
}

void DeviceParser::Cancel(std::vector<DeviceSample>& out) {
    if (touch_) {
        touch_->Cancel(scratchContacts_);
        EmitContacts(out);
    }
    pendingButtons_.clear();
    pendingScroll_ = {0.0f, 0.0f};
}

void DeviceParser::EmitContacts(std::vector<DeviceSample>& out) {
    for (const TouchContact& c : scratchContacts_) {
        out.push_back(DeviceSample{c});
    }
    scratchContacts_.clear();
}

void DeviceParser::FlushNonTouch(std::vector<DeviceSample>& out) {
    if (pointer_ && std::exchange(pointerMoved_, false)) {
        out.push_back(DeviceSample{DeviceSample::Hover{
            pointer_->x.Normalize(pointerPos_[0]),
            pointer_->y.Normalize(pointerPos_[1])}});
    }
    for (const auto& btn : pendingButtons_) {
        out.push_back(DeviceSample{btn});
    }
    pendingButtons_.clear();

    const float sx = std::exchange(pendingScroll_[0], 0.0f);
    const float sy = std::exchange(pendingScroll_[1], 0.0f);
    if (sx != 0.0f || sy != 0.0f) {
        out.push_back(DeviceSample{DeviceSample::Wheel{sx, sy}});
    }
}

} // namespace esp4all
