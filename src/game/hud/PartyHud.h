#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"

#include "game/audio/LevelSoundscape.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
namespace gdl::game {
/** Owns the party's HUD artwork, help text, pickup cards and selectors.
 * Borrows the glow texture until clear(); help refers to this owner's string table.
 * Canvas lifetime and screen overlay ordering belong to the caller, not this HUD. */
class PartyHud {
public:
    static constexpr s32 kPlayerCount = 4;
    static constexpr s32 kRelicTicks = 300; ///< welcome_timer: the keys show this long

private:
    struct Simulation {
        f32 localStopTimeTotal = 0;
        PickupHud pickups;
        HelpMessages help;
        std::optional<Vec3> helpPosition;
        std::array<PowerupSelector, kPlayerCount> selectors;
        s32 relicTicks = 0;
    };

public:
    class LocalSnapshot {
        friend class PartyHud;
        Simulation m_state;
        const PartyHud* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        ChallengeHud::LocalSnapshot m_hourglass;
        f32* m_sharedTotal = nullptr;
        f32 m_total = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    PartyHud() = default;
    ~PartyHud() = default;
    PartyHud(const PartyHud&) = delete;
    PartyHud& operator=(const PartyHud&) = delete;
    PartyHud(PartyHud&&) = delete;
    PartyHud& operator=(PartyHud&&) = delete;
    bool load(RenderDevice& device, const std::filesystem::path& root, const StringTable* strings);
    void clear();
    void setControlLabels(const ControlLabels* labels) { m_boxes.setControlLabels(labels); }
    void setGlow(const Texture* texture) { m_glowSheet = texture; }
    void setCountTextures(TextureSet* textures) { m_boxes.setCountTextures(textures); }
    /** The optional shared total belongs to the application and survives level travel. */
    bool bindHourglass(RenderDevice& device, ItemArchive& archive, f32* sharedTotal = nullptr);
    void stepHourglass(f32 seconds, std::span<const PlayerRuntime> players);
    /** Stop Time uses the secret-level timer's artwork and takes precedence while worn. */
    bool drawHourglass(Canvas& canvas, std::span<const PlayerRuntime> players) const;
    std::optional<ChallengeHud::Look> hourglassLook(std::span<const PlayerRuntime> players) const;
    void stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks,
                      LevelSoundscape& audio, bool challenge = false);
    void focusPickup(const PlayerActor& actor, s32 kind, u32 flags);
    bool postHelp(s32 id, usize index, std::span<PlayerRuntime> players, LevelSoundscape& audio,
                  s32 number = -1, std::optional<Vec3> position = std::nullopt);
    static StatusBoxView status(s32 player, std::span<const PlayerRuntime> players,
                                const PowerupSelector* selector = nullptr);
    /** The boxes; a fallen player's offers Quit Game only when `quitOffered`. */
    void drawStatus(Canvas& canvas, std::span<const PlayerRuntime> players,
                    bool quitOffered = true);
    /** Shows the bosses' keys in the boxes a while (a level opening, a runestone found). */
    void showRelics() { m_sim.relicTicks = kRelicTicks; }
    void stepRelics(s32 ticks) { m_sim.relicTicks = std::max(m_sim.relicTicks - ticks, 0); }
    bool relicsShown() const { return m_sim.relicTicks > 0; }
    void drawSelectors(Canvas& canvas, const TextPainter& text, const StringTable* strings,
                       std::span<const PlayerRuntime> players) const;
    static void drawSelector(Canvas& canvas, const TextPainter& text, const StringTable* strings,
                             const Texture* glow, s32 player, const PowerupSlot& slot, s32 y);
    void drawHelp(Canvas& canvas, RenderDevice& device, TextureSet& textures,
                  std::span<const PlayerRuntime> players, const Mat4& clip,
                  const Mat4& canvasProjection, f32 width, f32 height) const;
    const PowerupSelector& selector(s32 player) const {
        return m_sim.selectors[static_cast<usize>(std::clamp(player, 0, kPlayerCount - 1))];
    }
    PickupHud& pickups() { return m_sim.pickups; }
    const PickupHud& pickups() const { return m_sim.pickups; }
    HelpMessages& help() { return m_sim.help; }
    const HelpMessages& help() const { return m_sim.help; }
    std::optional<Vec3> helpAnchor(std::span<const PlayerRuntime> players) const;
    const MessageTable& strings() const { return m_strings; }

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    f32& stopTimeTotal() {
        return m_sharedStopTimeTotal != nullptr ? *m_sharedStopTimeTotal : m_sim.localStopTimeTotal;
    }
    f32 stopTimeTotal() const {
        return m_sharedStopTimeTotal != nullptr ? *m_sharedStopTimeTotal : m_sim.localStopTimeTotal;
    }
    StatusBoxPainter m_boxes;
    ChallengeHud m_hourglass;
    f32* m_sharedStopTimeTotal = nullptr;
    MessageTable m_strings;
    const Texture* m_glowSheet = nullptr;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
