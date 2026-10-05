#pragma once
#include <algorithm>
#include <array>
#include <optional>
#include <span>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"

#include "game/screens/ChallengeHud.h"
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
    static constexpr s32 kRelicTicks = 300; ///< welcome_timer: the keys show this long
    PartyHud() = default;
    ~PartyHud() = default;
    PartyHud(const PartyHud&) = delete;
    PartyHud& operator=(const PartyHud&) = delete;
    PartyHud(PartyHud&&) = delete;
    PartyHud& operator=(PartyHud&&) = delete;
    bool load(RenderDevice& device, const std::filesystem::path& root, const StringTable* strings);
    void clear();
    void setControlLabels(const ControlLabels& labels) { m_boxes.setControlLabels(labels); }
    void setGlow(const Texture* texture) { m_glowSheet = texture; }
    void setCountTextures(TextureSet* textures) { m_boxes.setCountTextures(textures); }
    /** The optional shared total belongs to the application and survives level travel. */
    bool bindHourglass(RenderDevice& device, ItemArchive& archive, f32* sharedTotal = nullptr);
    void stepHourglass(f32 seconds, std::span<const PlayerRuntime> players);
    /** Stop Time uses the secret-level timer's artwork and takes precedence while worn. */
    bool drawHourglass(Canvas& canvas, std::span<const PlayerRuntime> players) const;
    void stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks,
                      LevelSoundscape& audio);
    void focusPickup(const PlayerActor& actor, s32 kind, u32 flags);
    bool postHelp(s32 id, usize index, std::span<PlayerRuntime> players, LevelSoundscape& audio,
                  s32 number = -1, std::optional<Vec3> position = std::nullopt);
    static StatusBoxView status(s32 player, std::span<const PlayerRuntime> players,
                                const PowerupSelector* selector = nullptr);
    void drawStatus(Canvas& canvas, std::span<const PlayerRuntime> players);
    /** Shows the bosses' keys in the boxes a while (a level opening, a runestone found). */
    void showRelics() { m_relicTicks = kRelicTicks; }
    void stepRelics(s32 ticks) { m_relicTicks = std::max(m_relicTicks - ticks, 0); }
    bool relicsShown() const { return m_relicTicks > 0; }
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
    f32& stopTimeTotal() {
        return m_sharedStopTimeTotal != nullptr ? *m_sharedStopTimeTotal : m_localStopTimeTotal;
    }
    f32 stopTimeTotal() const {
        return m_sharedStopTimeTotal != nullptr ? *m_sharedStopTimeTotal : m_localStopTimeTotal;
    }
    StatusBoxPainter m_boxes;
    ChallengeHud m_hourglass;
    f32 m_localStopTimeTotal = 0;
    f32* m_sharedStopTimeTotal = nullptr;
    PickupHud m_pickups;
    HelpMessages m_help;
    std::optional<Vec3> m_helpPosition;
    MessageTable m_strings;
    std::array<PowerupSelector, kPlayerCount> m_selectors;
    const Texture* m_glowSheet = nullptr;
    s32 m_relicTicks = 0;
};
} // namespace gdl::game
