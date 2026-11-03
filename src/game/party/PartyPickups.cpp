#include "game/party/PartyPickups.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>

#include "engine/core/Types.h"

#include "game/characters/NameCheats.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/PickupHud.h"
#include "game/party/PickupPresentation.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {

constexpr s32 kSumnerVoice = 2; ///< change_player makes Sumner a wizard
constexpr std::string_view kFirstRuneVoice = "S_RUNEFOUND1";
constexpr std::string_view kRuneVoicePrefix = "S_RUNE"; ///< then S_RUNE2 to S_RUNE12
constexpr std::string_view kRunesFoundVoice = "S_RUNEFOUND2";
constexpr s32 kMostRunesCounted = 12;
constexpr std::string_view kLevelScrollPrefix = "SCROLLS"; ///< a level's scroll pages
constexpr std::string_view kUnlockLevel = "UNLOCKLEVEL";
constexpr std::string_view kUnlockSection = "UNLOCKSECTION";
/** The tower's word on a wing opened by the gargoyles' pieces (fn_8009C5B8). */
constexpr std::array<std::string_view, Relics::kGargoyleKinds> kUnlockSectionVoices{
    "S_FNGS4WST", "S_FTHS4WST", "S_CLWS4BTL"};
constexpr s32 kGoldLesson = 17; ///< COLLECTGOLD, for a pile of 25 or more
constexpr s32 kGoldLessonAmount = 25;
constexpr s32 kKeyLessonGates = 8; ///< SAVEKEYS, where there are gates to spend keys on
constexpr std::array<s32, 3> kPotionLessons{7, 94, 95}; ///< the first of them not yet told
constexpr std::string_view kSecretCoinCard = "COINHUD";
constexpr std::string_view kDroppedKey = "KEY"; ///< what a fallen player's keys lie as
constexpr std::string_view kDroppedKeyRing = "KEYRING";
constexpr std::array<std::string_view, Relics::kGargoyleKinds> kGargoyleIcons{
    "SM_FANGS", "SM_FEATHERS", "SM_CLAWS"};
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};

} // namespace

bool PartyPickups::storesTurbo(f32 held) {
    return held >= TurboMeter::kFull * kTurboStoreShare;
}

namespace {

/** Whether taking `kind` is shown with the pickup's gesture (items.c 3157-3385). */
bool picksUp(ItemKind kind) {
    return kind == ItemKind::Gold || kind == ItemKind::Keys || kind == ItemKind::Food ||
           kind == ItemKind::Potion ||
           (kind >= ItemKind::WeaponPowerup && kind <= ItemKind::SpecialPowerup);
}

/** Whether a pickup is the Turbo special, which fills the turbo meter the moment it works. */
bool turboPickup(const Pickup& pickup) {
    return pickup.subtype == static_cast<s32>(ItemKind::SpecialPowerup) &&
           (pickup.flags & powerup::kTurbo) != 0;
}

ItemTaking judgePickup(CharacterSave& save, const Pickup& pickup, const LevelWorld& world,
                       const ClassDataSet& classes, bool autoActivateItems, f32 turboHeld) {
    const ClassStats* stats = classes.stats(save.character);
    // A Turbo taken onto a meter all but full would be spent for nothing: it waits in the
    // inventory for the selector instead, as it does with Auto-Use off.
    const bool activate =
        autoActivateItems &&
        (!world.ref().isSecret() || powerupAllowedInChallenge(pickup.subtype, pickup.flags)) &&
        !(turboPickup(pickup) && PartyPickups::storesTurbo(turboHeld));
    auto taking =
        takeItem(save, ItemOffer{pickup.subtype, pickup.amount, pickup.flags, pickup.strength},
                 stats != nullptr ? stats->powerupTime : 1.0f, activate);
    if (pickup.subtype == static_cast<s32>(ItemKind::Gold) && world.ref().isSecret() &&
        taking.outcome == ItemTaking::Outcome::GoldFull) {
        // A challenge coin is still an objective when the wallet cannot accept gold.
        // Keep both the capped wallet and its lifetime gold total unchanged.
        taking = {.outcome = ItemTaking::Outcome::Taken,
                  .count = pickup.amount,
                  .card = kSecretCoinCard,
                  .sound = {}};
    }
    return taking;
}

std::optional<s32> refusalHelp(ItemTaking::Outcome outcome) {
    switch (outcome) {
    case ItemTaking::Outcome::KeysFull: return HelpMessages::kKeysFull;
    case ItemTaking::Outcome::PotionsFull: return HelpMessages::kPotionsFull;
    case ItemTaking::Outcome::HealthFull: return HelpMessages::kHealthFull;
    case ItemTaking::Outcome::AlreadyHeld: return HelpMessages::kAlreadyHaveRune;
    default: return std::nullopt;
    }
}

struct TheftComplaint {
    usize recipient = 0;
    std::string name;
};

std::optional<TheftComplaint> theftComplaint(s32 opener, s32 taker,
                                             std::span<const PlayerRuntime> players) {
    if (opener < 0 || opener == taker) {
        return std::nullopt;
    }
    for (usize index = 0; index < players.size(); ++index) {
        const auto& runtime = players[index];
        const CharacterSave& save = runtime.actor.save();
        if (runtime.actor.player() != opener || runtime.figure == nullptr ||
            PickupVoices::carriesPojo(save)) {
            continue;
        }
        const s32 voice =
            save.character == kSumnerClass ? kSumnerVoice : save.character % kStartingClassCount;
        return TheftComplaint{index, std::format("S_{}STEAL", classCode(voice))};
    }
    return std::nullopt;
}

} // namespace

std::vector<Collector> PartyPickups::collectors(std::span<const PlayerRuntime> players,
                                                const LevelWorld& world, const Chests& chests) {
    std::vector<Collector> result;
    result.reserve(players.size());
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        // Against an open chest, a character reaches what lies in it; the fallen reach
        // nothing, nor does one the console flies.
        const bool canCollect =
            runtime.life == PlayerLife::Standing && !runtime.departed && !runtime.ghost;
        const Vec3 here = canCollect ? actor.position() : kNowhere;
        const s32 chest = chests.holdingTouchedBy(ChestVisitor{here, actor.radius()});
        // PlayerMotion's first sItems sweep (8008760C) uses effectpos + dpos
        // before walls clip the step. effectpos is feet + PDAT coly, not the
        // floor position. Both offsets matter for crystals along a wall/ledge.
        Vec3 from =
            canCollect ? actor.stepAim() + (actor.followPoint() - actor.position()) : kNowhere;
        if (canCollect && ComboMove::flies(runtime.combo)) {
            // Pinball flight advances with slide(), not an ordinary input step;
            // it must not retain the last walk's attempted target.
            from = actor.followPoint();
        }
        if (chest >= 0) {
            const s32 held = chests.chest(static_cast<usize>(chest)).held;
            from = world.placedItems().item(static_cast<usize>(held)).position;
        }
        result.push_back(Collector{from, actor.reach(), actor.height() * 0.5f});
    }
    return result;
}

PickupDecision PartyPickups::takeCurrency(const Pickup& pickup, std::span<PlayerRuntime> players,
                                          const LevelWorld& world, const LevelFixtures& fixtures,
                                          const ClassDataSet& classes,
                                          const CurrencyPublisher& publish) {
    const auto kind = static_cast<ItemKind>(pickup.subtype);
    if (pickup.realm > 0 || pickup.collector >= players.size() || !publish ||
        (kind != ItemKind::Gold && kind != ItemKind::Keys)) {
        return {.accepted = false};
    }
    using Event = CurrencyEvent;
    const auto event = [&](Event::Kind type, std::string_view name = {}, s32 lesson = -1) {
        return publish({type, pickup.item, pickup.collector, lesson, std::string(name)});
    };
    const auto help = [&](s32 lesson) { return event(Event::Kind::Help, {}, lesson); };
    auto& runtime = players[pickup.collector];
    auto& actor = runtime.actor;
    // Gold and keys do not use the selector activation policy.
    const auto taking = judgePickup(actor.save(), pickup, world, classes, false, 0.0f);
    if (!taking.took()) {
        if (const auto lesson = refusalHelp(taking.outcome); lesson && !help(*lesson)) {
            return {.accepted = false};
        }
        return {};
    }
    if (!world.isTower()) {
        runtime.gesture = PlayerDeed::Pick;
    }
    const auto theft = [&] {
        const auto complaint = theftComplaint(pickup.opener, actor.player(), players);
        return !complaint || publish({Event::Kind::CharacterBark, pickup.item, complaint->recipient,
                                      -1, complaint->name});
    };
    const bool secretCoin = kind == ItemKind::Gold && world.ref().isSecret();
    if (kind == ItemKind::Gold) {
        if (!event(Event::Kind::ChallengeCoin) ||
            (!world.ref().isSecret() && pickup.amount >= kGoldLessonAmount &&
             (!help(kGoldLesson) || !theft()))) {
            return {.accepted = false};
        }
    } else if (!help(fixtures.gates().size() > 0 ? kKeyLessonGates
                                                 : HelpMessages::kChestNeedsKey) ||
               (taking.left == 0 && !theft())) {
        return {.accepted = false};
    }
    if (!taking.card.empty() &&
        !event(Event::Kind::Card, secretCoin ? kSecretCoinCard : taking.card)) {
        return {.accepted = false};
    }
    if (taking.message >= 0 && !help(taking.message)) {
        return {.accepted = false};
    }
    if (secretCoin) {
        if (!event(Event::Kind::NamedSound,
                   PickupVoices::bonusGold(actor.player(), pickup.amount))) {
            return {.accepted = false};
        }
    } else if (!taking.sound.empty() && !event(Event::Kind::NamedSound, taking.sound)) {
        return {.accepted = false};
    }
    return {.left = taking.left};
}

void PartyPickups::cleanupCollectedChests(const LevelWorld& world, Chests& chests) {
    for (usize chest = 0; chest < chests.size(); ++chest) {
        const s32 held = chests.chest(chest).held;
        if (held >= 0 && world.placedItems().item(static_cast<usize>(held)).taken) {
            chests.remove(chest);
        }
    }
}

/** Takes what the party stands on: a crystal counts for everyone, towards its realm's gate,
 * up to what the gate wants; the taker's box gets the card, every box the count. */
void PartyPickups::collect(RenderDevice& device, std::span<PlayerRuntime> players,
                           const Services& services) {
    LevelWorld& world = services.world;
    Chests& chests = services.fixtures.chests();
    const auto visitors = collectors(players, world, chests);
    const std::vector<Pickup> pickups = world.collect(
        device, visitors, [&](const Pickup& pickup) { return take(pickup, players, services); });
    cleanupCollectedChests(world, chests);
    for (const Pickup& pickup : pickups) {
        if (pickup.realm <= 0) {
            continue; // handed over as it was judged
        }
        const auto audioScope = services.audio.soundScope({35, pickup.item, 1}, 1);
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
                    services.presentation.showCount(
                        actor.player(), PickupHud::crystalIcon(pickup.realm), count, wanted);
                }
                enough = enough || (wanted > 0 && (count < 0 || count >= wanted));
            }
            if (pickup.collector < players.size()) {
                services.presentation.addCard(players[pickup.collector].actor.player(),
                                              PickupHud::kCrystalCard);
            }
            // TowerCheckMessages owns both the scroll and its persisted acknowledgement
            // in the tower. The immediate pickup notice is only for collections elsewhere.
            if (enough && !alreadyEnough && !world.isTower()) {
                announceUnlock(pickup.realm, players, services);
            }
        }
        services.audio.playPickup();
    }
}

void PartyPickups::beginTaking(const ItemOffer& offer, const ItemTaking& taking,
                               PlayerRuntime& runtime, const Services& services) {
    // Gold, keys, potions, good food and powerups are picked up with a gesture,
    // out of the tower; bad food is gagged on anywhere.
    const auto kind = static_cast<ItemKind>(offer.kind);
    if (kind >= ItemKind::WeaponPowerup && kind <= ItemKind::SpecialPowerup) {
        services.presentation.focusPickup(runtime.actor, offer.kind, offer.flags);
    }
    if (taking.hurt) {
        runtime.gesture = PlayerDeed::Gag;
    } else if (!services.world.isTower() && picksUp(kind)) {
        runtime.gesture = PlayerDeed::Pick;
    }
}

void PartyPickups::finishTaking(const ItemTaking& taking, std::string_view itemName,
                                usize recipient, std::span<PlayerRuntime> players,
                                const Services& services) {
    auto& runtime = players[recipient];
    auto& actor = runtime.actor;
    if (!taking.card.empty()) {
        services.presentation.addCard(actor.player(), taking.card);
    }
    if (taking.message >= 0 && services.help) {
        services.help(taking.message, recipient);
    }
    if (!taking.sound.empty()) {
        services.audio.playNamed(taking.sound);
    } else if (PlayerFigure* figure = runtime.figure.get();
               (taking.ate || taking.hurt) && figure != nullptr) {
        const bool pojo = PickupVoices::carriesPojo(actor.save());
        const PickupVoice cue =
            m_pickupVoices.food(actor.save().character, itemName, taking.hurt, pojo);
        if (cue.common) {
            services.audio.barkNamed(cue.sound);
        } else {
            services.audio.bark(figure->voice(), cue.sound);
        }
    }
}

void PartyPickups::presentTaken(const ItemOffer& offer, const ItemTaking& taking,
                                std::string_view itemName, usize recipient,
                                std::span<PlayerRuntime> players, const Services& services) {
    if (!taking.took() || recipient >= players.size() || players[recipient].departed) {
        return;
    }
    beginTaking(offer, taking, players[recipient], services);
    // Native campaign awards deliberately use no GDL subtype. This feedback
    // path must never call takeItem, shareRune, or a GDL progression ceremony.
    if (services.help) {
        switch (static_cast<ItemKind>(offer.kind)) {
        case ItemKind::Gold:
            if (offer.amount >= kGoldLessonAmount) {
                services.help(kGoldLesson, recipient);
            }
            break;
        case ItemKind::Keys:
            services.help(services.fixtures.gates().size() > 0 ? kKeyLessonGates
                                                               : HelpMessages::kChestNeedsKey,
                          recipient);
            break;
        case ItemKind::Potion:
            for (const s32 lesson : kPotionLessons) {
                if (services.help(lesson, recipient)) {
                    break;
                }
            }
            break;
        default: break;
        }
    }
    finishTaking(taking, itemName, recipient, players, services);
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
    // Item slots are append-only within a loaded epoch. Another pickup's
    // corrected-away sound must not renumber this item's event identity.
    const auto audioScope = services.audio.soundScope(
        {35, pickup.item, 1, static_cast<u8>(players[pickup.collector].actor.player())}, 2);
    if (pickup.subtype == static_cast<s32>(ItemKind::Gold) ||
        pickup.subtype == static_cast<s32>(ItemKind::Keys)) {
        return takeCurrency(pickup, players, services.world, services.fixtures, services.classes,
                            [&](const CurrencyEvent& event) {
                                const auto& recipient = players[event.recipient];
                                switch (event.kind) {
                                case CurrencyEvent::Kind::Help:
                                    if (services.help) {
                                        services.help(event.lesson, event.recipient);
                                    }
                                    break;
                                case CurrencyEvent::Kind::ChallengeCoin:
                                    if (services.challengeCoin) {
                                        services.challengeCoin(event.item);
                                    }
                                    break;
                                case CurrencyEvent::Kind::Card:
                                    services.presentation.addCard(recipient.actor.player(),
                                                                  event.name);
                                    break;
                                case CurrencyEvent::Kind::NamedSound:
                                    services.audio.playNamed(event.name);
                                    break;
                                case CurrencyEvent::Kind::CharacterBark:
                                    services.audio.bark(recipient.figure->voice(), event.name);
                                    break;
                                }
                                return true;
                            })
            .left;
    }
    const LevelWorld& world = services.world;
    PlayerRuntime& runtime = players[pickup.collector];
    PlayerActor& actor = runtime.actor;
    const auto help = [&](s32 id) { return services.help && services.help(id, pickup.collector); };
    const auto taking = judgePickup(actor.save(), pickup, world, services.classes,
                                    services.autoActivateItems, runtime.turbo.held());
    if (!taking.took()) {
        if (const auto id = refusalHelp(taking.outcome)) {
            help(*id);
        }
        return std::nullopt;
    }
    const auto kind = static_cast<ItemKind>(pickup.subtype);
    beginTaking({pickup.subtype, pickup.amount, pickup.flags, pickup.strength}, taking, runtime,
                services);
    switch (kind) {
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
    case ItemKind::GargoyleKey: {
        // The collector was credited by takeItem; other living participants share
        // one piece, while every recipient sees their own progress toward the statue.
        const auto piece = static_cast<usize>(pickup.amount);
        for (PlayerRuntime& member : players) {
            if (member.departed || member.life != PlayerLife::Standing) {
                continue;
            }
            Relics& relics = member.actor.save().progress().relics;
            if (&member != &runtime) {
                relics.addGargoylePiece(pickup.amount);
            }
            services.presentation.showCount(member.actor.player(), kGargoyleIcons[piece],
                                            relics.gargoylePieces[piece],
                                            Relics::kGargoyleNeeded[piece]);
        }
        break;
    }
    case ItemKind::Scroll:
        if (const LevelInfo* level = world.level();
            level != nullptr && taking.count >= 0 && services.openMessage) {
            services.openMessage(std::format("{}{}", kLevelScrollPrefix, level->name),
                                 static_cast<usize>(taking.count));
        }
        break;
    default: break;
    }
    const std::string_view itemName =
        (taking.ate || taking.hurt) && taking.sound.empty() && runtime.figure != nullptr
            ? world.placedItems().item(pickup.item).name
            : std::string_view{};
    finishTaking(taking, itemName, pickup.collector, players, services);
    return taking.left;
}

/** Whoever opened the chest a pickup came out of says so when another takes it
 * (fn_8009F748: the class's S_<CLS>STEAL, the unlockables their shadow's, Sumner a
 * wizard's; not while Pojo is carried), in the characters' turn. */
void PartyPickups::complainOfTheft(s32 opener, s32 taker, std::span<const PlayerRuntime> players,
                                   const Services& services) {
    if (const auto complaint = theftComplaint(opener, taker, players)) {
        services.audio.bark(players[complaint->recipient].figure->voice(), complaint->name);
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
    services.presentation.showRelics(); // welcome_timer again (items.c 3402)
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

bool PartyPickups::announceTowerUnlock(std::span<PlayerRuntime> players, const Services& services) {
    const auto participates = [](const PlayerRuntime& runtime) {
        const auto& save = runtime.actor.save();
        const auto* costume = hiddenCostume(save.name);
        return !runtime.departed && save.character != kSumnerClass &&
               (costume == nullptr || costume->character != save.character ||
                costume->directory != "SUM");
    };
    // TowerCheckMessages looks to the gargoyles' pieces first (completion record A: the
    // fangs, feathers and claws), then to completion2[0..7], excluding the black-crystal
    // entry. A negative count is the native, persisted "already congratulated" sentinel.
    for (usize kind = 0; kind < Relics::kGargoyleKinds; ++kind) {
        const s32 needed = Relics::kGargoyleNeeded[kind];
        s32 best = 0;
        for (const auto& runtime : players) {
            if (participates(runtime)) {
                const s32 count = runtime.actor.save().progress().relics.gargoylePieces[kind];
                if (best >= 0 && (count < 0 || count > best)) {
                    best = count;
                }
            }
        }
        if (best != needed || !services.openMessage ||
            !services.openMessage(kUnlockSection, kind)) {
            continue;
        }
        services.audio.speakOverScroll(kUnlockSectionVoices[kind]);
        for (auto& runtime : players) {
            if (participates(runtime) &&
                runtime.actor.save().progress().relics.gargoylePieces[kind] == needed) {
                runtime.actor.save().progress().relics.gargoylePieces[kind] = -1;
                const s32 character = runtime.actor.save().character;
                runtime.entrySave.classes[static_cast<usize>(character)]
                    .relics.gargoylePieces[kind] = -1;
            }
        }
        return true;
    }
    for (usize realm = 1; realm < 8; ++realm) {
        const s32 needed = LevelTriggers::crystalsNeeded(static_cast<s32>(realm));
        s32 best = 0;
        for (const auto& runtime : players) {
            if (participates(runtime)) {
                const s32 count = runtime.actor.save().progress().crystals[realm];
                if (best >= 0 && (count < 0 || count > best)) {
                    best = count;
                }
            }
        }
        if (best != needed || !services.openMessage || !services.openMessage(kUnlockLevel, realm)) {
            continue;
        }
        services.audio.speakOverScroll(kUnlockVoices[realm]);
        for (auto& runtime : players) {
            if (participates(runtime) &&
                runtime.actor.save().progress().crystals[realm] == needed) {
                auto& progress = runtime.actor.save().progress();
                progress.crystals[realm] = -1;
                progress.unlocked |= 1U << static_cast<u32>(realm);
                const s32 character = runtime.actor.save().character;
                auto& entry = runtime.entrySave.classes[static_cast<usize>(character)];
                entry.crystals[realm] = -1;
                entry.unlocked |= 1U << static_cast<u32>(realm);
            }
        }
        return true;
    }
    return false;
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
