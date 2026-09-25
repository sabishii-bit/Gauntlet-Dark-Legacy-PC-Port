#pragma once
#include <algorithm>
#include <array>
#include <span>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"

#include "game/screens/HelpMessages.h"
#include "game/screens/PickupHud.h"
#include "game/screens/PlayerRuntime.h"
#include "game/screens/PowerupSelector.h"
#include "game/screens/StatusBox.h"
#include "game/world/LevelSoundscape.h"
namespace gdl::game {
/** Owns the party's HUD artwork, help text, pickup cards and selectors.
 * Borrows the glow texture until clear(); help refers to this owner's string table.
 * Canvas lifetime and screen overlay ordering belong to the caller, not this HUD. */
class PartyHud {
public:
    static constexpr s32 kPlayerCount = 4;
    PartyHud() = default;
    ~PartyHud() = default;
    PartyHud(const PartyHud&) = delete;
    PartyHud& operator=(const PartyHud&) = delete;
    PartyHud(PartyHud&&) = delete;
    PartyHud& operator=(PartyHud&&) = delete;
    bool load(RenderDevice& device, const std::filesystem::path& root, const StringTable* strings);
    void clear();
    void setGlow(const Texture* texture) { m_glowSheet = texture; }
    void setCountTextures(TextureSet* textures) { m_boxes.setCountTextures(textures); }
    void stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks,
                      LevelSoundscape& audio);
    bool postHelp(s32 id, usize index, std::span<PlayerRuntime> players, LevelSoundscape& audio,
                  s32 number = -1);
    static StatusBoxView status(s32 player, std::span<const PlayerRuntime> players);
    void drawStatus(Canvas& canvas, std::span<const PlayerRuntime> players);
    void drawSelectors(Canvas& canvas, const TextPainter& text, const StringTable* strings,
                       std::span<const PlayerRuntime> players) const;
    void drawHelp(Canvas& canvas, RenderDevice& device, TextureSet& textures,
                  std::span<const PlayerRuntime> players, const Mat4& clip, f32 width,
                  f32 height) const;
    const PowerupSelector& selector(s32 player) const {
        return m_selectors[static_cast<usize>(std::clamp(player, 0, kPlayerCount - 1))];
    }
    PickupHud& pickups() { return m_pickups; }
    const PickupHud& pickups() const { return m_pickups; }
    HelpMessages& help() { return m_help; }
    const HelpMessages& help() const { return m_help; }
    const MessageTable& strings() const { return m_strings; }

private:
    StatusBoxPainter m_boxes;
    PickupHud m_pickups;
    HelpMessages m_help;
    MessageTable m_strings;
    std::array<PowerupSelector, kPlayerCount> m_selectors;
    const Texture* m_glowSheet = nullptr;
};
} // namespace gdl::game
