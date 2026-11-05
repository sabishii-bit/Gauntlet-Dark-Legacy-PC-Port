#include "game/widgets/SceneInput.h"

#include <algorithm>

namespace gdl::game {

SceneInput::SceneInput(const Input& devices, const GameConfig& config)
    : m_devices(devices), m_config(config) {}

MenuInputSource SceneInput::source(s32 inputSlot) const {
    return playerInputSource(m_devices, m_config, inputSlot);
}

MenuInput SceneInput::slot(s32 inputSlot) const {
    return readPlayerMenuInput(m_devices, m_config, inputSlot);
}

MenuInputs SceneInput::slots() const {
    MenuInputs inputs{};
    for (usize index = 0; index < inputs.size(); ++index) {
        inputs[index] = slot(static_cast<s32>(index));
    }
    return inputs;
}

MenuInputs SceneInput::typing(s32 typist) const {
    MenuInputs inputs{};
    for (usize index = 0; index < inputs.size(); ++index) {
        const auto inputSlot = static_cast<s32>(index);
        auto assigned = source(inputSlot);
        if (typist >= 0) {
            // Text entry temporarily borrows the keyboard; gameplay assignments stay intact.
            assigned.keyboard = inputSlot == typist;
            assigned.text = inputSlot == typist;
        }
        inputs[index] = readMenuInput(m_devices, menuBindings(m_config, inputSlot), assigned);
    }
    return inputs;
}

MenuInputs SceneInput::joining(const std::array<bool, kInputSlots>& admitted) const {
    MenuInputs inputs{};
    for (usize index = 0; index < inputs.size(); ++index) {
        if (admitted[index]) {
            inputs[index].start = slot(static_cast<s32>(index)).start;
        }
    }
    return inputs;
}

MenuInput SceneInput::shared() const {
    return readSharedMenuInput(m_devices, m_config);
}

MenuInput SceneInput::pause(s32 owner) const {
    return readPauseMenuInput(m_devices, m_config, owner);
}

MenuInput SceneInput::movie(std::optional<s32> inputSlot) const {
    return readMovieMenuInput(m_devices, m_config, inputSlot);
}

bool SceneInput::movieVote(s32 inputSlot) const {
    return readOnlineMovieSkipInput(m_devices, m_config, inputSlot);
}

bool SceneInput::quitPressed() const {
    return readMenuInput(m_devices, m_config.menu).escape;
}

bool SceneInput::recoveryPressed() const {
    return m_devices.wasKeyPressed(kRecoveryKey);
}

bool SceneInput::pressed(s32 inputSlot, std::span<const Key> keys,
                         std::span<const PadButton> buttons) const {
    const auto assigned = source(inputSlot);
    const auto keyPressed = [&](Key key) {
        // Mouse buttons belong to hit testing, whatever a profile binds them to.
        return key != Key::MouseLeft && key != Key::MouseRight && m_devices.wasKeyPressed(key);
    };
    const auto buttonPressed = [&](PadButton button) {
        return m_devices.wasPadButtonPressed(assigned.pad, button);
    };
    return (assigned.keyboard && std::ranges::any_of(keys, keyPressed)) ||
           std::ranges::any_of(buttons, buttonPressed);
}

} // namespace gdl::game
