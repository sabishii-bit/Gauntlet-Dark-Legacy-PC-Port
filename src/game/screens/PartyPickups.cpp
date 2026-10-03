#include "game/screens/PartyPickups.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>

#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PickupHud.h"

namespace gdl::game {

namespace {

constexpr s32 kSumnerVoice = 2; ///< change_player makes Sumner a wizard
constexpr std::string_view kFirstRuneVoice = "S_RUNEFOUND1";
constexpr std::string_view kRuneVoicePrefix = "S_RUNE"; ///< then S_RUNE2 to S_RUNE12
constexpr std::string_view kRunesFoundVoice = "S_RUNEFOUND2";
constexpr s32 kMostRunesCounted = 12;
constexpr std::string_view kLevelScrollPrefix = "SCROLLS"; ///< a level's scroll pages
constexpr std::string_view kUnlockLevel = "UNLOCKLEVEL";
constexpr s32 kSpecialPowerup = 9;  ///< the pickup subtype of the specials
constexpr u32 kTurboFlag = 0x80000; ///< of them, the one that fills the turbo meter
constexpr s32 kGoldLesson = 17;     ///< COLLECTGOLD, for a pile of 25 or more
constexpr s32 kGoldLessonAmount = 25;
constexpr s32 kKeyLessonGates = 8; ///< SAVEKEYS, where there are gates to spend keys on
constexpr std::array<s32, 3> kPotionLessons{7, 94, 95}; ///< the first of them not yet told
constexpr std::string_view kSecretCoinCard = "COINHUD";
constexpr std::string_view kDroppedKey = "KEY"; ///< what a fallen player's keys lie as
constexpr std::string_view kDroppedKeyRing = "KEYRING";
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};

/** Whether taking `kind` is shown with the pickup's gesture (items.c 3157-3385). */
bool picksUp(ItemKind kind) {
    return kind == ItemKind::Gold || kind == ItemKind::Keys || kind == ItemKind::Food ||
           kind == ItemKind::Potion ||
           (kind >= ItemKind::WeaponPowerup && kind <= ItemKind::SpecialPowerup);
}

} // namespace

/** Takes what the party stands on: a crystal counts for everyone, towards its realm's gate,
 * up to what the gate wants; the taker's box gets the card, every box the count. */
void PartyPickups::collect(RenderDevice& device, std::span<PlayerRuntime> players,
                           const Services& services) {
    LevelWorld& world = services.world;
    Chests& chests = services.fixtures.chests();
    std::vector<Collector> collectors;
    collectors.reserve(players.size());
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        // Against an open chest, a character reaches what lies in it; the fallen reach nothing.
        const Vec3 here =
            runtime.life == PlayerLife::Standing && !runtime.departed ? actor.position() : kNowhere;
        const s32 chest = chests.holdingTouchedBy(ChestVisitor{here, actor.radius()});
        Vec3 from = here;
        if (chest >= 0) {
            const s32 held = chests.chest(static_cast<usize>(chest)).held;
            from = world.placedItems().item(static_cast<usize>(held)).position;
        }
        collectors.push_back(Collector{from, actor.reach(), actor.height() * 0.5f});
    }
    const std::vector<Pickup> pickups = world.collect(
        device, collectors, [&](const Pickup& pickup) { return take(pickup, players, services); });
    for (usize chest = 0; chest < chests.size(); ++chest) {
        const s32 held = chests.chest(chest).held;
        if (held >= 0 && world.placedItems().item(static_cast<usize>(held)).taken) {
            chests.remove(chest);
        }
    }
    for (const Pickup& pickup : pickups) {
        if (pickup.realm <= 0) {
            continue; // handed over as it was judged
        }
        if (static_cast<usize>(pickup.realm) < kRealmCount) {
            const s32 wanted = LevelTriggers::crystalsNeeded(pickup.realm);
            bool alreadyEnough = false;
            bool enough = false;
            for (PlayerRuntime& runtime : players) {
                if (runtime.departed) {
                    continue;
                }
                PlayerActor& actor = runtime.actor;
                s32& count = actor.save().progress().crystals[static_cast<usize>(pickup.realm)];
                // Retail retains negative completed records and opens a gate when any
                // participant meets its requirement (towerAdvanceLevelRecord and
                // towerAllPlayersMetLevelReq, likewise the boss-record variants).
                alreadyEnough = alreadyEnough || (wanted > 0 && (count < 0 || count >= wanted));
                if (runtime.life == PlayerLife::Standing) {
                    // towerAdvanceLevelRecord awards only states 1/4, while the
                    // requirement query still considers records of the fallen.
                    if (count >= 0 && (wanted <= 0 || count < wanted)) {
                        ++count;
                    }
                    services.hud.pickups().showCount(
                        actor.player(), PickupHud::crystalIcon(pickup.realm), count, wanted);
                }
                enough = enough || (wanted > 0 && (count < 0 || count >= wanted));
            }
            if (pickup.collector < players.size()) {
                services.hud.pickups().addCard(players[pickup.collector].actor.player(),
                                               PickupHud::kCrystalCard);
            }
            if (enough && !alreadyEnough) {
                announceUnlock(pickup.realm, players, services);
            }
        }
        services.audio.playPickup();
    }
}

/** Hands a touched item to whoever touched it; nothing when they leave it lying, else what is
 * left of its amount. Crystals are the party's and are dealt with once taken. */
std::optional<s32> PartyPickups::take(const Pickup& pickup, std::span<PlayerRuntime> players,
                                      const Services& services) {
    if (pickup.realm > 0) {
        return 0;
    }
    if (pickup.collector >= players.size()) {
        return std::nullopt;
    }
    const LevelWorld& world = services.world;
    PlayerRuntime& runtime = players[pickup.collector];
    PlayerActor& actor = runtime.actor;
    const auto help = [&](s32 id) { return services.help && services.help(id, pickup.collector); };
    const bool secretCoin =
        pickup.subtype == static_cast<s32>(ItemKind::Gold) && world.ref().isSecret();
    const ClassStats* stats = services.classes.stats(actor.save().character);
    const ItemTaking taking = takeItem(
        actor.save(), ItemOffer{pickup.subtype, pickup.amount, pickup.flags, pickup.strength},
        stats != nullptr ? stats->powerupTime : 1.0f);
    if (!taking.took()) {
        switch (taking.outcome) {
        case ItemTaking::Outcome::KeysFull: help(HelpMessages::kKeysFull); break;
        case ItemTaking::Outcome::PotionsFull: help(HelpMessages::kPotionsFull); break;
        case ItemTaking::Outcome::HealthFull: help(HelpMessages::kHealthFull); break;
        case ItemTaking::Outcome::AlreadyHeld: help(HelpMessages::kAlreadyHaveRune); break;
        default: break;
        }
        return std::nullopt;
    }
    if (pickup.subtype == kSpecialPowerup && (static_cast<u32>(pickup.flags) & kTurboFlag) != 0) {
        runtime.turbo.add(TurboMeter::kFull);
    }
    // Gold, keys, potions, good food and powerups are picked up with a gesture, out of the
    // tower; bad food is gagged on anywhere.
    const auto kind = static_cast<ItemKind>(pickup.subtype);
    if (taking.hurt) {
        runtime.gesture = PlayerDeed::Gag;
    } else if (!world.isTower() && picksUp(kind)) {
        runtime.gesture = PlayerDeed::Pick;
    }
    switch (kind) {
    case ItemKind::Gold:
        if (services.challengeCoin) {
            services.challengeCoin(pickup.item);
        }
        if (!world.ref().isSecret() && pickup.amount >= kGoldLessonAmount) {
            help(kGoldLesson);
            complainOfTheft(pickup.opener, actor.player(), players, services);
        }
        break;
    case ItemKind::Keys:
        help(services.fixtures.gates().size() > 0 ? kKeyLessonGates : HelpMessages::kChestNeedsKey);
        if (taking.left == 0) {
            complainOfTheft(pickup.opener, actor.player(), players, services);
        }
        break;
    case ItemKind::Potion:
        complainOfTheft(pickup.opener, actor.player(), players, services);
        // Retail tries the next lesson only when the previous one could not be posted.
        for (const s32 lesson : kPotionLessons) {
            if (help(lesson)) {
                break;
            }
        }
        break;
    case ItemKind::Food: complainOfTheft(pickup.opener, actor.player(), players, services); break;
    case ItemKind::Runestone: shareRune(pickup.amount, players, services); break;
    case ItemKind::Legend: help(HelpMessages::kFirstLegendName + taking.count); break;
    case ItemKind::Scroll:
        if (const LevelInfo* level = world.level();
            level != nullptr && taking.count >= 0 && services.openMessage) {
            services.openMessage(std::format("{}{}", kLevelScrollPrefix, level->name),
                                 static_cast<usize>(taking.count));
        }
        break;
    default: break;
    }
    if (!taking.card.empty()) {
        // Retail do_got_it selects COINHUD in realm 12 before testing gold value.
        services.hud.pickups().addCard(actor.player(), secretCoin ? kSecretCoinCard : taking.card);
    }
    if (taking.message >= 0) {
        help(taking.message);
    }
    if (!taking.sound.empty()) {
        if (secretCoin) {
            services.audio.playNamed(PickupVoices::bonusGold(actor.player(), pickup.amount));
        } else {
            services.audio.playNamed(taking.sound);
        }
    } else if (PlayerFigure* figure = runtime.figure.get();
               (taking.ate || taking.hurt) && figure != nullptr && services.sounds != nullptr) {
        const bool pojo = PickupVoices::carriesPojo(actor.save());
        const PickupVoice cue = m_pickupVoices.food(
            actor.save().character, world.placedItems().item(pickup.item).name, taking.hurt, pojo);
        if (cue.common) {
            services.audio.barkNamed(cue.sound);
        } else {
            services.audio.bark(figure->voice(), cue.sound);
        }
    }
    return taking.left;
}

/** Whoever opened the chest a pickup came out of says so when another takes it
 * (fn_8009F748: the class's S_<CLS>STEAL, the unlockables their shadow's, Sumner a
 * wizard's; not while Pojo is carried), in the characters' turn. */
void PartyPickups::complainOfTheft(s32 opener, s32 taker, std::span<const PlayerRuntime> players,
                                   const Services& services) {
    if (opener < 0 || opener == taker) {
        return;
    }
    for (const PlayerRuntime& runtime : players) {
        const CharacterSave& save = runtime.actor.save();
        if (runtime.actor.player() != opener || runtime.figure == nullptr ||
            PickupVoices::carriesPojo(save)) {
            continue;
        }
        const s32 voice =
            save.character == kSumnerClass ? kSumnerVoice : save.character % kStartingClassCount;
        services.audio.bark(runtime.figure->voice(), std::format("S_{}STEAL", classCode(voice)));
        return;
    }
}

/** Living characters share a runestone (PlayerGiveShard's states 1/4). The narrator also
 * counts existing runes held by fallen characters still waiting in the party. */
void PartyPickups::shareRune(s32 rune, std::span<PlayerRuntime> players, const Services& services) {
    u16 held = 0;
    for (PlayerRuntime& runtime : players) {
        if (runtime.departed) {
            continue;
        }
        Relics& relics = runtime.actor.save().progress().relics;
        if (runtime.life == PlayerLife::Standing) {
            relics.addRune(rune);
        }
        held |= relics.runes;
    }
    services.hud.showRelics(); // welcome_timer again (items.c 3402)
    services.audio.duckMusic(services.audio.lengthOf("S_PICKUPRUNE") -
                                 LevelSoundscape::kRuneMusicLead,
                             LevelSoundscape::kRuneMusicScale);
    for (const std::string& voice : runeCountVoices(std::popcount(held))) {
        services.audio.queueNarration(voice, LevelSoundscape::Narrator::Primary);
    }
}

std::vector<std::string> PartyPickups::runeCountVoices(s32 count) {
    if (count <= 0 || count > kMostRunesCounted) {
        return {};
    }
    if (count == 1) {
        return {std::string(kFirstRuneVoice)};
    }
    return {std::format("{}{}", kRuneVoicePrefix, count), std::string(kRunesFoundVoice)};
}

/** Congratulates the party once its crystals open a realm's gate: the scroll for the realm,
 * its announcing voice, and the save remembers so it is not said twice. */
void PartyPickups::announceUnlock(s32 realm, std::span<PlayerRuntime> players,
                                  const Services& services) {
    if (realm <= 0 || static_cast<usize>(realm) >= kRealmCount) {
        return;
    }
    const u32 bit = 1U << static_cast<u32>(realm);
    bool fresh = false;
    for (PlayerRuntime& runtime : players) {
        if (runtime.departed) {
            continue;
        }
        ClassProgress& progress = runtime.actor.save().progress();
        fresh = fresh || (progress.unlocked & bit) == 0;
        progress.unlocked |= bit;
    }
    if (!fresh) {
        return;
    }
    if (services.openMessage) {
        services.openMessage(kUnlockLevel, static_cast<usize>(realm));
    }
    if (static_cast<usize>(realm) < kUnlockVoices.size()) {
        services.audio.speakOverScroll(kUnlockVoices[static_cast<usize>(realm)]);
    }
}

void PartyPickups::dropKeys(RenderDevice& device, LevelWorld& world, PlayerRuntime& runtime) {
    if (world.isTower()) {
        return;
    }
    const LevelInfo* level = world.level();
    if (level != nullptr && level->bossType >= 0) {
        return;
    }
    Inventory& inventory = runtime.actor.save().progress().inventory;
    if (inventory.keys <= 0) {
        return;
    }
    const std::string_view name = inventory.keys == 1 ? kDroppedKey : kDroppedKeyRing;
    const auto& infos = world.layout().itemInfos();
    const auto record = std::ranges::find_if(infos, [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup &&
               info.subtype == static_cast<s32>(ItemKind::Keys) && info.name == name;
    });
    if (record != infos.end() &&
        world.placeItemRecord(device, static_cast<s32>(std::distance(infos.begin(), record)),
                              runtime.actor.position(), inventory.keys)) {
        inventory.keys = 0;
    }
}

} // namespace gdl::game
