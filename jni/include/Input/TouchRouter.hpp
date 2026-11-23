#pragma once

#include "Input/TouchParser.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace esp4all {

enum class TouchTarget : uint8_t { Interface, Outside };

struct UiRect {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;

    bool Contains(std::array<float, 2> pos) const noexcept {
        return pos[0] >= minX && pos[0] <= maxX && pos[1] >= minY && pos[1] <= maxY;
    }
};

// Snapshot of visible UI windows in display pixels published after each ImGui render.
struct InterfaceView {
    std::array<float, 2> size{0.0f, 0.0f};
    DisplayRotation rotation = DisplayRotation::Deg0;
    std::vector<UiRect> regions;
    bool dismissable = false;

    TouchTarget FindTarget(std::array<float, 2> position) const noexcept;
};

struct InputEvent {
    struct Touch {
        uint32_t device = 0;
        uint32_t id = 0;
        TouchPhase phase = TouchPhase::Start;
        std::array<float, 2> position{0.0f, 0.0f};
    };

    struct PointerMoved {
        std::array<float, 2> position{0.0f, 0.0f};
    };

    struct PointerButton {
        std::array<float, 2> position{0.0f, 0.0f};
        MouseButton button = MouseButton::Left;
        bool pressed = false;
    };

    struct PointerGone {};

    struct Wheel {
        float x = 0.0f;
        float y = 0.0f;
    };

    std::variant<Touch, PointerMoved, PointerButton, PointerGone, Wheel> value;

    bool IsButton(bool pressed) const noexcept;
    bool IsLift() const noexcept;
    bool IsPointerMotion() const noexcept;
    bool IsMotion() const noexcept;
    bool IsSameMotion(const InputEvent& other) const noexcept;
};

/**
 * Classifies each finger ONCE where it lands (Phase::Start):
 *  - Fingers landing on the DebugUI belong to the overlay for their entire gesture
 *    (preventing clicks from leaking into the game underneath when uinput control is on).
 *  - Fingers landing outside belong to the game for their entire gesture
 *    (preventing in-game aim drags from accidentally pulling the overlay menu).
 */
class TouchRouter {
public:
    TouchTarget RouteContact(
        uint32_t device,
        TouchContact contact,
        std::array<float, 2> position,
        const InterfaceView& view,
        std::vector<InputEvent>& out);

    void RouteHover(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out);
    void RouteButton(MouseButton which, bool pressed, const InterfaceView& view, std::vector<InputEvent>& out);
    void RouteWheel(float x, float y, const InterfaceView& view, std::vector<InputEvent>& out);

private:
    bool IsMouseDragging() const noexcept;
    void DismissPopups(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out);

    std::unordered_map<uint64_t, TouchTarget> routes_;
    std::optional<uint64_t> activeFinger_;
    std::array<float, 2> pointerPos_{0.0f, 0.0f};
    bool hovering_ = false;
    std::array<bool, 3> heldButtons_{false, false, false};
};

// Coalesces consecutive motion events (1000 Hz panels) without dropping presses/releases,
// and cuts events cleanly per frame so ImGui sees press -> drag -> release in separate frames.
class EventQueue {
public:
    bool IsEmpty() const noexcept { return events_.empty(); }
    void Push(InputEvent event, std::chrono::steady_clock::time_point at);
    std::optional<std::chrono::steady_clock::time_point> TakeFrame(std::vector<InputEvent>& into);

private:
    std::vector<InputEvent> events_;
    std::vector<std::chrono::steady_clock::time_point> times_;
};

} // namespace esp4all
