#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gdl {

enum class Key : std::uint8_t {
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
enum class PadButton : std::uint8_t {
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

enum class PadAxis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count
};

/** State of one gamepad for one frame. */
struct PadSnapshot {
    bool connected = false;
    std::array<bool, static_cast<std::size_t>(PadButton::Count)> buttons{};
    std::array<float, static_cast<std::size_t>(PadAxis::Count)> axes{};
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
    static constexpr int kMaxPads = 4;

    bool isKeyDown(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;

    bool isPadConnected(int pad) const;
    bool isPadButtonDown(int pad, PadButton button) const;
    bool wasPadButtonPressed(int pad, PadButton button) const;
    /** Sticks report -1..1, triggers 0..1. */
    float padAxis(int pad, PadAxis axis) const;

    /** Characters typed since the last poll, as Unicode code points in order. */
    std::span<const std::uint32_t> typedText() const { return m_typed; }

    /** Platform-layer entry points. */
    void beginPoll();
    void setKey(Key key, bool down);
    /** Records a press seen since the last poll; it holds the key down until the next. */
    void latchKey(Key key);
    void setPad(int pad, const PadSnapshot& snapshot);
    void addTypedChar(std::uint32_t codepoint);

private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);

    bool keyDown(std::size_t key) const { return m_keys[key] || m_latchedKeys[key]; }

    std::array<bool, kKeyCount> m_keys{};
    std::array<bool, kKeyCount> m_latchedKeys{};
    std::array<bool, kKeyCount> m_previousKeys{};
    std::array<PadSnapshot, kMaxPads> m_pads{};
    std::array<PadSnapshot, kMaxPads> m_previousPads{};
    std::vector<std::uint32_t> m_typed;
};

} // namespace gdl
