#include "engine/platform/Input.h"

#include <cctype>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl {

namespace {

constexpr usize index(Key key) {
    return static_cast<usize>(key);
}
constexpr usize index(PadButton button) {
    return static_cast<usize>(button);
}
constexpr usize index(PadAxis axis) {
    return static_cast<usize>(axis);
}

constexpr bool validKey(Key key) {
    return key != Key::Unknown && key < Key::Count;
}
constexpr bool validPad(s32 pad) {
    return pad >= 0 && pad < Input::kMaxPads;
}

constexpr std::array<std::string_view, static_cast<usize>(Key::Count)> kKeyNames{
    "",      "Escape",    "Enter",       "Space",   "Tab", "Backspace", "Up", "Down", "Left",
    "Right", "LeftShift", "LeftControl", "LeftAlt", "A",   "B",         "C",  "D",    "E",
    "F",     "G",         "H",           "I",       "J",   "K",         "L",  "M",    "N",
    "O",     "P",         "Q",           "R",       "S",   "T",         "U",  "V",    "W",
    "X",     "Y",         "Z",           "0",       "1",   "2",         "3",  "4",    "5",
    "6",     "7",         "8",           "9",       "F1",  "F2",        "F3", "F4",   "F5",
    "F6",    "F7",        "F8",          "F9",      "F10", "F11",       "F12"};

constexpr std::array<std::string_view, static_cast<usize>(PadButton::Count)> kPadButtonNames{
    "A",         "B",        "X",        "Y",           "LeftBumper",  "RightBumper",
    "Back",      "Start",    "Guide",    "LeftThumb",   "RightThumb",  "DpadUp",
    "DpadRight", "DpadDown", "DpadLeft", "LeftTrigger", "RightTrigger"};

bool sameIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view keyName(Key key) {
    return validKey(key) ? kKeyNames[index(key)] : std::string_view{};
}

std::optional<Key> keyFromName(std::string_view name) {
    for (usize i = 1; i < kKeyNames.size(); ++i) {
        if (sameIgnoringCase(kKeyNames[i], name)) {
            return static_cast<Key>(i);
        }
    }
    return std::nullopt;
}

std::string_view padButtonName(PadButton button) {
    return button < PadButton::Count ? kPadButtonNames[index(button)] : std::string_view{};
}

std::optional<PadButton> padButtonFromName(std::string_view name) {
    for (usize i = 0; i < kPadButtonNames.size(); ++i) {
        if (sameIgnoringCase(kPadButtonNames[i], name)) {
            return static_cast<PadButton>(i);
        }
    }
    return std::nullopt;
}

bool Input::isKeyDown(Key key) const {
    return validKey(key) && keyDown(index(key));
}

bool Input::wasKeyPressed(Key key) const {
    return validKey(key) && keyDown(index(key)) && !m_previousKeys[index(key)];
}

bool Input::wasKeyReleased(Key key) const {
    return validKey(key) && !keyDown(index(key)) && m_previousKeys[index(key)];
}

bool Input::isPadConnected(s32 pad) const {
    return validPad(pad) && m_pads[pad].connected;
}

bool Input::isPadButtonDown(s32 pad, PadButton button) const {
    return isPadConnected(pad) && button < PadButton::Count && m_pads[pad].buttons[index(button)];
}

bool Input::wasPadButtonPressed(s32 pad, PadButton button) const {
    return isPadButtonDown(pad, button) && !m_previousPads[pad].buttons[index(button)];
}

f32 Input::padAxis(s32 pad, PadAxis axis) const {
    return validPad(pad) ? m_pads[pad].axes[index(axis)] : 0.0f;
}

void Input::beginPoll() {
    for (usize key = 0; key < kKeyCount; ++key) {
        m_previousKeys[key] = keyDown(key);
    }
    m_latchedKeys.fill(false);
    m_previousPads = m_pads;
    m_typed.clear();
}

void Input::setKey(Key key, bool down) {
    if (validKey(key)) {
        m_keys[index(key)] = down;
    }
}

void Input::latchKey(Key key) {
    if (validKey(key)) {
        m_latchedKeys[index(key)] = true;
    }
}

void Input::setPad(s32 pad, const PadSnapshot& snapshot) {
    if (validPad(pad)) {
        const PadSnapshot previous = m_pads[pad];
        m_pads[pad] = snapshot.connected ? snapshot : PadSnapshot{};
        for (const auto button : {PadButton::LeftTrigger, PadButton::RightTrigger}) {
            const PadAxis axis =
                button == PadButton::LeftTrigger ? PadAxis::LeftTrigger : PadAxis::RightTrigger;
            const f32 threshold =
                previous.connected && previous.buttons[index(button)] ? 0.4f : 0.5f;
            m_pads[pad].buttons[index(button)] =
                snapshot.connected && snapshot.axes[index(axis)] >= threshold;
        }
    }
}

void Input::addTypedChar(u32 codepoint) {
    m_typed.push_back(codepoint);
}

} // namespace gdl
