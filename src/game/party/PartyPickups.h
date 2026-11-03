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

#include "game/audio/LevelSoundscape.h"
#include "game/characters/ClassData.h"
#include "game/level/LevelFixtures.h"
#include "game/level/LevelWorld.h"
#include "game/party/PickupPresentation.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/ItemPickup.h"
#include "game/players/PickupVoices.h"

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
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const PartyPickups& value) {
        const auto& [pickupVoices] = value;
        visitor.value(pickupVoices);
    }

public:
    bool operator==(const PartyPickups&) const = default;

    /** The voice that announces each realm's gate opening, by realm. */
    static constexpr std::array<std::string_view, 9> kUnlockVoices{
        "",           "S_CRYS4TWN", "S_CRYS4MNT", "S_CRYS4CST", "S_CRYS4SKY",
        "S_CRYS4FOR", "S_CRYS4DES", "S_CRYS4ICE", "S_CRYS4DRM"};

    /** What the pickups borrow of the scene for one frame. */
    struct Services {
        LevelWorld& world;
        LevelFixtures& fixtures;
        PickupPresentation& presentation; ///< the cards, counts, selectors and keys
        LevelSoundscape& audio;
        const ClassDataSet& classes;
        SoundPlayer* sounds = nullptr;
        std::function<bool(s32, usize)> help;                     ///< a lesson for a member
        std::function<bool(std::string_view, usize)> openMessage; ///< a scroll page
        std::function<void(usize)> challengeCoin;                 ///< a coin of the challenge
        bool autoActivateItems = true;
    };

    /** The share of a full turbo meter from which a Turbo pickup is stored rather than
     * used: Auto-Use would spend it to fill a meter that is all but full already. */
    static constexpr f32 kTurboStoreShare = 0.9f;
    /** Whether a Turbo pickup taken with Auto-Use on waits in the inventory, the meter at
     * `held` of `TurboMeter::kFull` being nine tenths full or more. */
    static bool storesTurbo(f32 held);

    /** Takes what the party stands on (an open chest's contents from beside it). */
    void collect(RenderDevice& device, std::span<PlayerRuntime> players, const Services& services);
    /** Present a completed native grant without applying it again or sharing GDL relics.
     * Recipient is the party index; an empty offer is a native campaign award. */
    void presentTaken(const ItemOffer& offer, const ItemTaking& taking, std::string_view itemName,
                      usize recipient, std::span<PlayerRuntime> players, const Services& services);
    /** Exact native reach, including an opened container's held pickup anchor. */
    static std::vector<Collector> collectors(std::span<const PlayerRuntime> players,
                                             const LevelWorld& world, const Chests& chests);
    struct CurrencyEvent {
        enum class Kind { Help, ChallengeCoin, Card, NamedSound, CharacterBark };
        Kind kind = Kind::Help;
        usize item = 0;
        usize recipient = 0; ///< party index, not input slot
        s32 lesson = -1;
        std::string name;
    };
    using CurrencyPublisher = std::function<bool(const CurrencyEvent&)>;
    /** Native Gold/Keys inventory, gesture and ordered feedback dispatch. A
     * publisher's false means transaction refusal, not a help-display result.
     * Earlier mutations must be rewound by the caller on refusal. Other kinds
     * and crystals are refused without mutation; no renderer/audio is invoked. */
    static PickupDecision takeCurrency(const Pickup& pickup, std::span<PlayerRuntime> players,
                                       const LevelWorld& world, const LevelFixtures& fixtures,
                                       const ClassDataSet& classes,
                                       const CurrencyPublisher& publish);
    /** Native post-collection container cleanup, without pickup/audio callbacks. */
    static void cleanupCollectedChests(const LevelWorld& world, Chests& chests);

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
    static void beginTaking(const ItemOffer& offer, const ItemTaking& taking,
                            PlayerRuntime& runtime, const Services& services);
    void finishTaking(const ItemTaking& taking, std::string_view itemName, usize recipient,
                      std::span<PlayerRuntime> players, const Services& services);
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
