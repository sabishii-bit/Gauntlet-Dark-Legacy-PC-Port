#pragma once

#include <array>

#include "engine/core/Types.h"

namespace gdl {

enum class Key : u8 {
    Unknown = 0,
    Escape,
    Enter,
    Space,
    Tab,
    Backspace,
    Up,
    Down,
    Left,
    Right,
    LeftShift,
    LeftControl,
    LeftAlt,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Count
};

/** Gamepad buttons in the standard Xbox-style layout. */
enum class PadButton : u8 {
    A,
    B,
    X,
    Y,
    LeftBumper,
    RightBumper,
    Back,
    Start,
    Guide,
    LeftThumb,
    RightThumb,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Count
};

enum class PadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

/** State of one gamepad for one frame. */
struct PadSnapshot {
    bool connected = false;
    std::array<bool, static_cast<usize>(PadButton::Count)> buttons{};
    std::array<f32, static_cast<usize>(PadAxis::Count)> axes{};
};

/** Frame-coherent keyboard and gamepad state, updated once per Window::pollEvents(). */
class Input {
public:
    static constexpr int kMaxPads = 4;

    bool isKeyDown(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;

    bool isPadConnected(int pad) const;
    bool isPadButtonDown(int pad, PadButton button) const;
    bool wasPadButtonPressed(int pad, PadButton button) const;
    /** Sticks report -1..1, triggers 0..1. */
    f32 padAxis(int pad, PadAxis axis) const;

    /** Platform-layer entry points. */
    void beginPoll();
    void setKey(Key key, bool down);
    void setPad(int pad, const PadSnapshot& snapshot);

private:
    static constexpr usize kKeyCount = static_cast<usize>(Key::Count);

    std::array<bool, kKeyCount> m_keys{};
    std::array<bool, kKeyCount> m_previousKeys{};
    std::array<PadSnapshot, kMaxPads> m_pads{};
    std::array<PadSnapshot, kMaxPads> m_previousPads{};
};

} // namespace gdl
