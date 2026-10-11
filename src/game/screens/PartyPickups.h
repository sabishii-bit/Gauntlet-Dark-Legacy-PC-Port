#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"

#include "game/players/ClassData.h"
#include "game/players/PickupVoices.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/PartyHud.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/**
 * What the party picks up in a level: each touched item handed to whoever touched it by the
 * original's rules (their card slides up, the item's sound or their own eating plays, and what
 * they cannot carry stays lying), the pickup's gesture or a gag at bad food, the lessons the
 * item teaches, a chest's opener complaining when another takes what it held, runestones
 * shared and counted aloud, and crystals counted for everyone towards their realm's gate,
 * whose opening is announced once. It keeps no state of its own but the food voices.
 */
class PartyPickups {
public:
    /** The voice that announces each realm's gate opening, by realm. */
    static constexpr std::array<std::string_view, 9> kUnlockVoices{
        "",           "S_CRYS4TWN", "S_CRYS4MNT", "S_CRYS4CST", "S_CRYS4SKY",
        "S_CRYS4FOR", "S_CRYS4DES", "S_CRYS4ICE", "S_CRYS4DRM"};

    /** What the pickups borrow of the scene for one frame. */
    struct Services {
        LevelWorld& world;
        LevelFixtures& fixtures;
        PartyHud& hud;
        LevelSoundscape& audio;
        const ClassDataSet& classes;
        SoundPlayer* sounds = nullptr;
        std::function<bool(s32, usize)> help;                     ///< a lesson for a member
        std::function<bool(std::string_view, usize)> openMessage; ///< a scroll page
        std::function<void(usize)> challengeCoin;                 ///< a coin of the challenge
        bool autoActivateItems = true;
    };

    /** Takes what the party stands on (an open chest's contents from beside it). */
    void collect(RenderDevice& device, std::span<PlayerRuntime> players, const Services& services);

    /** TowerCheckMessages acknowledges crystal sets collected here or before returning to
     * the tower. Pickups elsewhere retain their immediate notice. True if a scroll opened. */
    static bool announceTowerUnlock(std::span<PlayerRuntime> players, const Services& services);

    /** What the narrator says of the party's runestones, in order (AudioNumRunesFound): the
     * first found, or the count and "runestones found"; nothing past twelve. */
    static std::vector<std::string> runeCountVoices(s32 count);
    /** A player's death played out, the keys they carried fall where they lay, one as a KEY
     * and more as a KEYRING holding them all, for the rest of the party (player_dies); not in
     * the tower nor where a boss is fought. */
    static void dropKeys(RenderDevice& device, LevelWorld& world, PlayerRuntime& runtime);

private:
    std::optional<s32> take(const Pickup& pickup, std::span<PlayerRuntime> players,
                            const Services& services);
    static void complainOfTheft(s32 opener, s32 taker, std::span<const PlayerRuntime> players,
                                const Services& services);
    static void shareRune(s32 rune, std::span<PlayerRuntime> players, const Services& services);
    static void announceUnlock(s32 realm, std::span<PlayerRuntime> players,
                               const Services& services);

    PickupVoices m_pickupVoices;
};

} // namespace gdl::game
