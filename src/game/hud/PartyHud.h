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
#include "game/players/PlayerControls.h"
namespace gdl::game {
/** Owns the party's help text, pickup cards and selectors, which the scene's HUD look draws.
 * Help refers to this owner's string table. */
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
    /** Loads the help messages' strings from the unpacked data, in `strings`' words. */
    void load(const std::filesystem::path& root, const StringTable* strings);
    void clear();
    /** The optional shared total belongs to the application and survives level travel. */
    bool bindHourglass(RenderDevice& device, ItemArchive& archive, f32* sharedTotal = nullptr);
    void stepHourglass(f32 seconds, std::span<const PlayerRuntime> players);
    /** Stop Time's sand while a standing player wears it, drawn in the secret-level timer's
     * artwork (hourglass()); it takes precedence over the secret level's own. */
    std::optional<ChallengeHud::Look> hourglassLook(std::span<const PlayerRuntime> players) const;
    /** Stop Time's hourglass, as bound and stepped, which draws hourglassLook()'s look. */
    const ChallengeHud& hourglass() const { return m_hourglass; }
    void stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks,
                      LevelSoundscape& audio, bool challenge = false);
    void focusPickup(const PlayerActor& actor, s32 kind, u32 flags);
    bool postHelp(s32 id, usize index, std::span<PlayerRuntime> players, LevelSoundscape& audio,
                  s32 number = -1, std::optional<Vec3> position = std::nullopt);
    static StatusBoxView status(s32 player, std::span<const PlayerRuntime> players,
                                const PowerupSelector* selector = nullptr);
    /** Shows the bosses' keys in the boxes a while (a level opening, a runestone found). */
    void showRelics() { m_sim.relicTicks = kRelicTicks; }
    void stepRelics(s32 ticks) { m_sim.relicTicks = std::max(m_sim.relicTicks - ticks, 0); }
    bool relicsShown() const { return m_sim.relicTicks > 0; }
    const PowerupSelector& selector(s32 player) const {
        return m_sim.selectors[static_cast<usize>(std::clamp(player, 0, kPlayerCount - 1))];
    }
    PickupHud& pickups() { return m_sim.pickups; }
    const PickupHud& pickups() const { return m_sim.pickups; }
    HelpMessages& help() { return m_sim.help; }
    const HelpMessages& help() const { return m_sim.help; }
    /** Where the help message up is anchored: its player's attention point, else its place. */
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
    ChallengeHud m_hourglass;
    f32* m_sharedStopTimeTotal = nullptr;
    MessageTable m_strings;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
