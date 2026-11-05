#pragma once

#include <array>
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/widgets/MenuInput.h"

namespace gdl::game {

/** One update's devices as the screens read them, through this machine's settings: each input
 * slot through its own devices and bindings, the shared reading for screens no player owns.
 * Borrows the devices and settings for one update and reads them when asked, so a settings
 * change made during the update is seen. Online screens that read the agreed room settings
 * pass them explicitly (AfterLevelScene::captureShopInput). */
class SceneInput {
public:
    /** Leaves a pause menu or the netplay overlay, whatever the bindings say. */
    static constexpr Key kRecoveryKey = Key::Escape;

    SceneInput(const Input& devices, const GameConfig& config);
    /** The settings are borrowed, so they may not be a temporary. */
    SceneInput(const Input&, const GameConfig&&) = delete;

    const Input& devices() const { return m_devices; }
    const GameConfig& config() const { return m_config; }

    MenuInputSource source(s32 inputSlot) const;
    MenuInput slot(s32 inputSlot) const;
    MenuInputs slots() const;
    /** Every slot, with the keyboard lent to `typist`'s text field; -1 lends it to nobody. */
    MenuInputs typing(s32 typist) const;
    /** Only the Start presses of the admitted slots. */
    MenuInputs joining(const std::array<bool, kInputSlots>& admitted) const;
    MenuInput shared() const;
    MenuInput pause(s32 owner) const;
    MenuInput movie(std::optional<s32> inputSlot = std::nullopt) const;
    bool movieVote(s32 inputSlot) const;
    /** The settings' global quit keys, not a slot's own. */
    bool quitPressed() const;
    bool recoveryPressed() const;
    /** A binding press on `inputSlot`'s own devices by the menus' rules; mouse buttons do not
     * count. */
    bool pressed(s32 inputSlot, std::span<const Key> keys,
                 std::span<const PadButton> buttons) const;

private:
    const Input& m_devices;
    const GameConfig& m_config;
};

} // namespace gdl::game
