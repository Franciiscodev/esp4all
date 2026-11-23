#include "Input/TouchRouter.hpp"

#include <algorithm>
#include <utility>

namespace esp4all {

bool InputEvent::IsButton(bool pressed) const noexcept {
    const auto* btn = std::get_if<PointerButton>(&value);
    return btn && btn->pressed == pressed;
}

bool InputEvent::IsLift() const noexcept {
    const auto* t = std::get_if<Touch>(&value);
    return t && (t->phase == TouchPhase::End || t->phase == TouchPhase::Cancel);
}

bool InputEvent::IsPointerMotion() const noexcept {
    return std::holds_alternative<PointerMoved>(value);
}

bool InputEvent::IsMotion() const noexcept {
    if (const auto* t = std::get_if<Touch>(&value)) {
        return t->phase == TouchPhase::Move;
    }
    return IsPointerMotion();
}

bool InputEvent::IsSameMotion(const InputEvent& other) const noexcept {
    if (!IsMotion() || !other.IsMotion()) {
        return false;
    }
    const auto* a = std::get_if<Touch>(&value);
    const auto* b = std::get_if<Touch>(&other.value);
    if (a && b) {
        return a->device == b->device && a->id == b->id;
    }
    return IsPointerMotion() && other.IsPointerMotion();
}

TouchTarget InterfaceView::FindTarget(std::array<float, 2> position) const noexcept {
    for (const UiRect& r : regions) {
        if (r.Contains(position)) {
            return TouchTarget::Interface;
        }
    }
    return TouchTarget::Outside;
}

TouchTarget TouchRouter::RouteContact(
    uint32_t device,
    TouchContact contact,
    std::array<float, 2> position,
    const InterfaceView& view,
    std::vector<InputEvent>& out)
{
    const uint64_t key = (static_cast<uint64_t>(device) << 32) | static_cast<uint64_t>(contact.id);
    const auto it = routes_.find(key);
    const std::optional<TouchTarget> known =
        (it == routes_.end()) ? std::nullopt : std::optional<TouchTarget>(it->second);

    const TouchTarget target = (contact.phase == TouchPhase::Start)
        ? view.FindTarget(position)
        : known.value_or(TouchTarget::Outside);

    switch (contact.phase) {
        case TouchPhase::Start:
            routes_[key] = target;
            if (target == TouchTarget::Interface) {
                out.push_back(InputEvent{InputEvent::Touch{device, contact.id, TouchPhase::Start, position}});
                if (!activeFinger_ && !IsMouseDragging()) {
                    activeFinger_ = key;
                    pointerPos_ = position;
                    out.push_back(InputEvent{InputEvent::PointerMoved{position}});
                    out.push_back(InputEvent{InputEvent::PointerButton{position, MouseButton::Left, true}});
                }
            } else {
                DismissPopups(position, view, out);
            }
            break;

        case TouchPhase::Move:
            if (known == TouchTarget::Interface) {
                out.push_back(InputEvent{InputEvent::Touch{device, contact.id, TouchPhase::Move, position}});
                if (activeFinger_ && *activeFinger_ == key) {
                    pointerPos_ = position;
                    out.push_back(InputEvent{InputEvent::PointerMoved{position}});
                }
            }
            break;

        case TouchPhase::End:
        case TouchPhase::Cancel:
            if (it != routes_.end()) {
                routes_.erase(it);
            }
            if (known == TouchTarget::Interface) {
                out.push_back(InputEvent{InputEvent::Touch{device, contact.id, contact.phase, position}});
                if (activeFinger_ && *activeFinger_ == key) {
                    activeFinger_.reset();
                    hovering_ = false;
                    if (contact.phase == TouchPhase::End) {
                        out.push_back(InputEvent{InputEvent::PointerButton{position, MouseButton::Left, false}});
                    }
                    out.push_back(InputEvent{InputEvent::PointerGone{}});
                }
            }
            break;
    }

    return target;
}

void TouchRouter::RouteHover(
    std::array<float, 2> position,
    const InterfaceView& view,
    std::vector<InputEvent>& out)
{
    pointerPos_ = position;
    if (activeFinger_) {
        return;
    }
    if (IsMouseDragging() || view.FindTarget(position) == TouchTarget::Interface) {
        hovering_ = true;
        out.push_back(InputEvent{InputEvent::PointerMoved{position}});
    } else if (std::exchange(hovering_, false)) {
        out.push_back(InputEvent{InputEvent::PointerGone{}});
    }
}

void TouchRouter::RouteButton(
    MouseButton which,
    bool pressed,
    const InterfaceView& view,
    std::vector<InputEvent>& out)
{
    const std::array<float, 2> pos = pointerPos_;
    const size_t idx = static_cast<size_t>(which);
    if (!pressed) {
        if (std::exchange(heldButtons_[idx], false)) {
            out.push_back(InputEvent{InputEvent::PointerButton{pos, which, false}});
        }
        return;
    }
    if (activeFinger_) {
        return;
    }
    if (view.FindTarget(pos) == TouchTarget::Interface) {
        heldButtons_[idx] = true;
        hovering_ = true;
        out.push_back(InputEvent{InputEvent::PointerMoved{pos}});
        out.push_back(InputEvent{InputEvent::PointerButton{pos, which, true}});
    } else if (which == MouseButton::Left) {
        DismissPopups(pos, view, out);
    }
}

void TouchRouter::RouteWheel(
    float x,
    float y,
    const InterfaceView& view,
    std::vector<InputEvent>& out)
{
    if (view.FindTarget(pointerPos_) == TouchTarget::Interface) {
        if (!activeFinger_ && !std::exchange(hovering_, true)) {
            out.push_back(InputEvent{InputEvent::PointerMoved{pointerPos_}});
        }
        out.push_back(InputEvent{InputEvent::Wheel{x, y}});
    }
}

bool TouchRouter::IsMouseDragging() const noexcept {
    return heldButtons_[0] || heldButtons_[1] || heldButtons_[2];
}

void TouchRouter::DismissPopups(
    std::array<float, 2> position,
    const InterfaceView& view,
    std::vector<InputEvent>& out)
{
    if (view.dismissable && !activeFinger_ && !IsMouseDragging()) {
        out.push_back(InputEvent{InputEvent::PointerMoved{position}});
        out.push_back(InputEvent{InputEvent::PointerButton{position, MouseButton::Left, true}});
        out.push_back(InputEvent{InputEvent::PointerButton{position, MouseButton::Left, false}});
        out.push_back(InputEvent{InputEvent::PointerGone{}});
        hovering_ = false;
    }
}

void EventQueue::Push(InputEvent event, std::chrono::steady_clock::time_point at) {
    if (event.IsMotion()) {
        for (size_t i = events_.size(); i > 0 && events_[i - 1].IsMotion(); --i) {
            if (events_[i - 1].IsSameMotion(event)) {
                events_[i - 1] = std::move(event);
                times_[i - 1] = at;
                return;
            }
        }
    }
    events_.push_back(std::move(event));
    times_.push_back(at);
}

std::optional<std::chrono::steady_clock::time_point> EventQueue::TakeFrame(std::vector<InputEvent>& into) {
    auto findFirst = [this](size_t limit, auto pred) -> std::optional<size_t> {
        for (size_t i = 0; i < limit; ++i) {
            if (pred(events_[i])) {
                return i;
            }
        }
        return std::nullopt;
    };

    const auto pressIdx = findFirst(events_.size(), [](const InputEvent& e) { return e.IsButton(true); });
    size_t end = pressIdx ? (*pressIdx + 1) : events_.size();

    const auto releaseIdx = findFirst(end, [](const InputEvent& e) { return e.IsButton(false); });
    if (releaseIdx) {
        size_t cut = *releaseIdx;
        if (cut > 0 && events_[cut - 1].IsLift()) {
            --cut;
        }
        if (findFirst(cut, [](const InputEvent& e) { return e.IsPointerMotion(); })) {
            end = cut;
        }
    }

    const auto count = static_cast<std::ptrdiff_t>(end);
    into.insert(into.end(),
                std::make_move_iterator(events_.begin()),
                std::make_move_iterator(events_.begin() + count));
    events_.erase(events_.begin(), events_.begin() + count);

    std::optional<std::chrono::steady_clock::time_point> latest;
    for (size_t i = 0; i < end; ++i) {
        latest = latest ? std::max(*latest, times_[i]) : times_[i];
    }
    times_.erase(times_.begin(), times_.begin() + count);
    return latest;
}

} // namespace esp4all
