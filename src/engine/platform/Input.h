#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

/** The configuration name of a key ("Enter", "A", "F1"), empty for Unknown/Count. */
std::string_view keyName(Key key);
/** The key a configuration name denotes, ignoring case. */
std::optional<Key> keyFromName(std::string_view name);
std::string_view padButtonName(PadButton button);
std::optional<PadButton> padButtonFromName(std::string_view name);

/** Frame-coherent keyboard and gamepad state, updated once per Window::pollEvents(). A key
 * pressed and released again between two polls still counts as down for the poll that
 * follows, so a tap shorter than a frame is never lost. */
class Input {
public:
    static constexpr s32 kMaxPads = 4;

    bool isKeyDown(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;

    bool isPadConnected(s32 pad) const;
    bool isPadButtonDown(s32 pad, PadButton button) const;
    bool wasPadButtonPressed(s32 pad, PadButton button) const;
    /** Sticks report -1..1, triggers 0..1. */
    f32 padAxis(s32 pad, PadAxis axis) const;

    /** Characters typed since the last poll, as Unicode code points in order. */
    std::span<const u32> typedText() const { return m_typed; }

    /** Platform-layer entry points. */
    void beginPoll();
    void setKey(Key key, bool down);
    /** Records a press seen since the last poll; it holds the key down until the next. */
    void latchKey(Key key);
    void setPad(s32 pad, const PadSnapshot& snapshot);
    void addTypedChar(u32 codepoint);

private:
    static constexpr usize kKeyCount = static_cast<usize>(Key::Count);

    bool keyDown(usize key) const { return m_keys[key] || m_latchedKeys[key]; }

    std::array<bool, kKeyCount> m_keys{};
    std::array<bool, kKeyCount> m_latchedKeys{};
    std::array<bool, kKeyCount> m_previousKeys{};
    std::array<PadSnapshot, kMaxPads> m_pads{};
    std::array<PadSnapshot, kMaxPads> m_previousPads{};
    std::vector<u32> m_typed;
};

} // namespace gdl
