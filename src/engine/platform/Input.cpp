#include "engine/platform/Input.h"

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
constexpr bool validPad(int pad) {
    return pad >= 0 && pad < Input::kMaxPads;
}

} // namespace

bool Input::isKeyDown(Key key) const {
    return validKey(key) && m_keys[index(key)];
}

bool Input::wasKeyPressed(Key key) const {
    return validKey(key) && m_keys[index(key)] && !m_previousKeys[index(key)];
}

bool Input::wasKeyReleased(Key key) const {
    return validKey(key) && !m_keys[index(key)] && m_previousKeys[index(key)];
}

bool Input::isPadConnected(int pad) const {
    return validPad(pad) && m_pads[pad].connected;
}

bool Input::isPadButtonDown(int pad, PadButton button) const {
    return validPad(pad) && m_pads[pad].buttons[index(button)];
}

bool Input::wasPadButtonPressed(int pad, PadButton button) const {
    return validPad(pad) && m_pads[pad].buttons[index(button)] &&
           !m_previousPads[pad].buttons[index(button)];
}

f32 Input::padAxis(int pad, PadAxis axis) const {
    return validPad(pad) ? m_pads[pad].axes[index(axis)] : 0.0f;
}

void Input::beginPoll() {
    m_previousKeys = m_keys;
    m_previousPads = m_pads;
}

void Input::setKey(Key key, bool down) {
    if (validKey(key)) {
        m_keys[index(key)] = down;
    }
}

void Input::setPad(int pad, const PadSnapshot& snapshot) {
    if (validPad(pad)) {
        m_pads[pad] = snapshot;
    }
}

} // namespace gdl
