#include "game/scenes/SharedConsoleVerbs.h"

#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <optional>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/Inventory.h"
#include "game/characters/Progression.h"
#include "game/console/ConsoleCommands.h"
#include "game/console/GiftRequest.h"
#include "game/console/SpawnRequest.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/level/TowerAccess.h"
#include "game/players/ItemPickup.h"
#include "game/players/PlayerActor.h"
#include "game/rollback/SharedConsoleWords.h"
#include "game/scenes/ConsoleReplies.h"
#include "game/scenes/ConsoleStage.h"

namespace gdl::game {
namespace {

using Arguments = SharedConsoleVerb::Arguments;
using ConsolePlay = PlayScene::ConsolePlay;
using Reach = gdl::Expected<ConsoleReach, ConsoleRefusal>;

constexpr s32 kMaximumGrant = 1000000;              ///< the most one console grant may give
constexpr f32 kMaximumPowerupCounter = 10000000.0f; ///< a powerup's charge or seconds a grant
                                                    ///< may take it to
constexpr f32 kMaximumCoordinate = 1.0e6f;          ///< a console place's farthest on any axis
constexpr f32 kAhead = 6.0f;                        ///< units before the character
constexpr f32 kApart = 3.0f;                        ///< between several
constexpr f32 kProbeAbove = 3.0f;
constexpr f32 kProbeBelow = 10.0f;

/** Words a parser could not read: its usage when that is all it says, else its message. */
ConsoleRefusal unreadable(std::string text, const std::string& usage) {
    const auto kind = text == usage ? ConsoleRefusal::Kind::Usage : ConsoleRefusal::Kind::Message;
    return {.kind = kind, .text = std::move(text)};
}

/** `result` after the notes there are, a line each. */
std::string afterNotes(std::span<const std::optional<std::string>> notes,
                       const std::string& result) {
    std::string reply;
    for (const std::optional<std::string>& note : notes) {
        if (note) {
            reply += *note;
            reply += '\n';
        }
    }
    return reply + result;
}

/** A usage's constant rows as rows that own their words. */
std::vector<std::pair<std::string, std::string>>
ownedRows(std::span<const std::pair<std::string_view, std::string_view>> rows) {
    std::vector<std::pair<std::string, std::string>> owned;
    owned.reserve(rows.size());
    for (const auto& [form, what] : rows) {
        owned.emplace_back(form, what);
    }
    return owned;
}

/** A command that reads no words after its name: every peer refuses any, with its name, before
 * they are sent; offline the words reach the verb, which says it takes none. */
class WordlessVerb : public SharedConsoleVerb {
public:
    bool takesArguments() const override { return false; }
    Reach check(Arguments arguments, ConsolePlay play) const override {
        if (!arguments.empty() && play == ConsolePlay::Shared) {
            return gdl::Unexpected(ConsoleRefusal{.kind = ConsoleRefusal::Kind::Message,
                                                  .text = std::format("Usage: {}", name())});
        }
        return ConsoleReach::Shared;
    }
};

/** god, notarget or noclip: turns one of the seat's cheats on, or off again. */
class CheatToggle final : public WordlessVerb {
public:
    CheatToggle(std::string_view name, bool PlayScene::ConsoleCheats::*flag,
                std::string_view summary)
        : m_name(name), m_flag(flag), m_summary(summary) {}

    std::string_view name() const override { return m_name; }
    std::string_view summaryTail() const override { return m_summary; }
    std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const override {
        if (!arguments.empty()) {
            return "This command toggles without arguments.";
        }
        PlayScene::ConsoleCheats changed = stage.cheats(seat);
        bool& enabled = changed.*m_flag;
        enabled = !enabled;
        stage.setCheats(seat, changed);
        return cheatReply(stage.play(), m_name, enabled);
    }

private:
    std::string_view m_name;
    bool PlayScene::ConsoleCheats::*m_flag;
    std::string_view m_summary;
};

/** get: an item, experience or a level for the seat's character; get info stays local. */
class GiftVerb final : public SharedConsoleVerb {
public:
    std::string_view name() const override { return "get"; }
    std::vector<std::string_view> summaryForms() const override {
        return {"<item> [amount]", "exp <points>", "level <level>"};
    }
    std::string_view summaryTail() const override { return "for you"; }
    std::vector<std::pair<std::string, std::string>> usageRows() const override {
        return giftUsageRows();
    }
    std::string_view usageNote() const override { return GiftRequest::kUsageNote; }
    bool takesArguments() const override { return true; }
    Reach check(Arguments arguments, ConsolePlay /*play*/) const override {
        const auto gift = parseGift(arguments);
        if (!gift) {
            return gdl::Unexpected(unreadable(gift.error(), giftUsage()));
        }
        return gift->kind == GiftRequest::Kind::Info ? ConsoleReach::Local : ConsoleReach::Shared;
    }
    std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const override {
        const auto gift = parseGift(arguments);
        if (!gift) {
            return gift.error();
        }
        if (gift->amount > kMaximumGrant || gift->offer.amount > kMaximumGrant ||
            !std::isfinite(gift->offer.strength) || gift->offer.strength > kMaximumGrant) {
            return "A console grant must be finite and no larger than 1000000.";
        }
        CharacterSave& save = stage.save(seat);
        switch (gift->kind) {
        case GiftRequest::Kind::Experience: {
            // PartyRecords applies native stage scaling and can share the award
            // with a combo partner. Check both additions before entering its s32
            // arithmetic; an untrusted console amount must never overflow it.
            const auto awarded = stage.fittingAward(save, gift->amount);
            if (!awarded) {
                return "That experience grant would exceed the character's counter.";
            }
            for (const CharacterSave* partner : stage.comboPartners(seat)) {
                if (!stage.fittingAward(*partner, *awarded)) {
                    return "That experience grant would exceed the partner's counter.";
                }
            }
            stage.awardExperience(seat, gift->amount);
            stage.restat(seat);
            return std::format("{} experience: level {}", gift->amount,
                               experienceLevel(save.experience()));
        }
        case GiftRequest::Kind::Level:
            save.progress().experience = levelExperience(gift->amount);
            stage.restat(seat);
            return std::format("Level {}", gift->amount);
        case GiftRequest::Kind::Item: {
            const auto& offer = gift->offer;
            const s64 amount = offer.amount;
            const s64 maximum = std::numeric_limits<s32>::max();
            if ((offer.kind == static_cast<s32>(ItemKind::Gold) &&
                 (static_cast<s64>(save.gold) + amount > maximum ||
                  static_cast<s64>(save.progress().lifetime.goldFound) + amount > maximum)) ||
                (offer.kind == static_cast<s32>(ItemKind::Food) &&
                 static_cast<s64>(save.health()) + amount > maximum)) {
                return "That item grant would exceed the character's counter.";
            }
            for (const auto& held : save.progress().inventory.powerups) {
                if (held.kind == offer.kind && held.flags == offer.flags &&
                    (!std::isfinite(held.charge + static_cast<f32>(offer.amount)) ||
                     !std::isfinite(held.strength + offer.strength * Inventory::kRenewShare) ||
                     held.charge + static_cast<f32>(offer.amount) > kMaximumPowerupCounter ||
                     held.strength + offer.strength * Inventory::kRenewShare >
                         kMaximumPowerupCounter)) {
                    return "That item grant would exceed the powerup's counter.";
                }
            }
            return giftReport(*gift, takeItem(save, gift->offer, 1.0f, true));
        }
        case GiftRequest::Kind::Info: return "get info is a local inspection command.";
        }
        return "Unknown shared console command.";
    }
};

/** goto: the party to a point here, or on to a level and a point there. */
class GotoVerb final : public SharedConsoleVerb {
public:
    std::string_view name() const override { return "goto"; }
    std::vector<std::string_view> summaryForms() const override {
        return {"<map> [x y z]", "x y z", "<legends map> [preview]"};
    }
    std::string_view summaryTail() const override { return "brings the party there"; }
    std::vector<std::pair<std::string, std::string>> usageRows() const override {
        return ownedRows(ConsoleCommands::kGotoUsageRows);
    }
    std::string_view usageNote() const override { return ConsoleCommands::kGotoUsageNote; }
    std::vector<std::string> completions(Arguments arguments,
                                         const LevelCatalog& levels) const override {
        if (arguments.size() > 1) {
            return {};
        }
        std::vector<std::string> words{"tower"};
        for (const LevelCatalog::Realm& realm : levels.realms()) {
            words.insert(words.end(), realm.levels.begin(), realm.levels.end());
        }
        return words;
    }
    bool takesArguments() const override { return true; }
    Reach check(Arguments arguments, ConsolePlay /*play*/) const override {
        const auto destination = ConsoleCommands::parseGoto(arguments);
        if (!destination) {
            return gdl::Unexpected(unreadable(destination.error(), ConsoleCommands::gotoUsage()));
        }
        return destination->offlineOnly() ? ConsoleReach::OfflineOnly : ConsoleReach::Shared;
    }
    std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const override {
        const auto destination = ConsoleCommands::parseGoto(arguments);
        if (!destination) {
            return destination.error();
        }
        // Every peer refuses alike what the shared game cannot go to.
        if (destination->offlineOnly()) {
            return std::string(offlineOnlyRefusal());
        }
        if (destination->position && !consolePointInRange(*destination->position)) {
            return "Coordinates must be finite and within +/-1000000.";
        }
        if (!destination->level) {
            if (!destination->position) {
                return ConsoleCommands::gotoUsage();
            }
            const Vec3& at = *destination->position;
            return movedReply(stage.play(), stage.relocate(at) ? std::optional{at} : std::nullopt);
        }
        const auto level = stage.levelNamed(*destination->level);
        if (!level) {
            return unknownMapReply(stage.play(), *destination->level);
        }
        stage.requestTravel({*level, destination->position, std::nullopt}, seat);
        return std::format("Going to {} ({})", level->name, level->title);
    }
    std::string_view offlineOnlyRefusal() const override {
        return "Legends maps are offline only.";
    }
};

/** spawn: the swarm, a great one, a generator, a pickup or an object before the seat. */
class SpawnVerb final : public SharedConsoleVerb {
public:
    std::string_view name() const override { return "spawn"; }
    std::string_view summaryTail() const override {
        return "stands enemies, great ones, generators, pickups or objects before you; spawn "
               "alone says how";
    }
    std::vector<std::pair<std::string, std::string>> usageRows() const override {
        return ownedRows(SpawnRequest::kUsageRows);
    }
    std::string_view usageNote() const override { return SpawnRequest::kUsageNote; }
    bool takesArguments() const override { return true; }
    Reach check(Arguments arguments, ConsolePlay /*play*/) const override {
        if (const auto spawn = parseSpawn(arguments); !spawn) {
            return gdl::Unexpected(unreadable(spawn.error(), spawnUsage()));
        }
        return ConsoleReach::Shared;
    }
    std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const override {
        const ConsolePlay play = stage.play();
        const PlayerActor* owner = stage.actor(seat);
        if (owner == nullptr) {
            return "Nobody is in to spawn before.";
        }
        const auto spawn = parseSpawn(arguments);
        if (!spawn) {
            return spawn.error();
        }
        if (!stage.spawnRoom()) {
            return "The stage's console spawn limit is reached.";
        }
        std::optional<LevelRef> from;
        if (spawn->map) {
            from = stage.levelNamed(*spawn->map);
            if (!from) {
                return unknownMapReply(play, *spawn->map);
            }
        }
        const Vec3 facing = owner->facing();
        const f32 yaw = owner->yaw() + std::numbers::pi_v<f32>;
        const auto spotAt = [&](s32 index) {
            Vec3 spot = owner->position() + facing * (kAhead + kApart * static_cast<f32>(index));
            if (const auto floor = stage.collision().floorAt(spot + Vec3{0.0f, kProbeAbove, 0.0f},
                                                             kProbeAbove, kProbeBelow)) {
                spot.y = floor->y;
            }
            return spot;
        };
        // What stood borrows the map's voices; offline the reply first says when it lends none.
        std::optional<std::string> voices;
        const auto borrowVoices = [&] {
            if (from && !stage.borrowVoices(*from)) {
                voices = voicesNote(play, from->name);
            }
        };
        using Kind = SpawnRequest::Kind;
        switch (spawn->kind) {
        case Kind::Enemy: {
            const auto views = stage.opponentViews();
            s32 count = 0;
            for (s32 i = 0; i < spawn->count && stage.spawnRoom(); ++i) {
                if (stage.enemies().loadKind(spawn->enemyKind) &&
                    stage.enemies().spawn({.kind = spawn->enemyKind,
                                           .tier = spawn->tier,
                                           .position = spotAt(i),
                                           .direction = -facing,
                                           .placed = true},
                                          views)) {
                    ++count;
                    stage.countSpawn();
                }
            }
            if (count > 0) {
                borrowVoices();
            }
            return afterNotes(std::array{voices},
                              spawnedEnemiesReply(play, count, spawn->name, spawn->tier));
        }
        case Kind::Critter: {
            const char realm = from && !from->name.empty() ? from->name.front() : '\0';
            std::optional<s32> id;
            switch (spawn->critter) {
            case CombatantKind::Golem:
                id = stage.critters().spawnGolem(spotAt(0), yaw, realm);
                break;
            case CombatantKind::General:
                id = stage.critters().spawnGeneral(spotAt(0), yaw, 0, realm);
                break;
            case CombatantKind::Gargoyle:
                id = stage.critters().spawnGargoyle(spotAt(0), yaw, spawn->form, realm);
                break;
            case CombatantKind::Unknown:
            case CombatantKind::Boss: break;
            }
            if (id) {
                stage.countSpawn();
            }
            return critterReply(play, id.has_value(), spawn->name, spawn->form);
        }
        case Kind::Generator: {
            s32 kind = spawn->enemyKind;
            ItemArchive* items = nullptr;
            WorldLayout layout;
            const ItemInfo* record = nullptr;
            std::optional<std::string> unpacked; ///< offline, that the map's layout is missing
            if (spawn->brood) {
                const s32 realm = from ? from->realmId : stage.level().realmId;
                if (realm != TowerAccess::kTempleWorld && realm != TowerAccess::kUnderworldWorld) {
                    return std::string(broodRealmRefusal(play));
                }
                kind = realm == TowerAccess::kTempleWorld ? Generators::kTempleBrood
                                                          : Generators::kHellBrood;
                if (from) {
                    items = stage.borrowItems(*from);
                    if (!items) {
                        return std::format("{} lends no item archive", from->name);
                    }
                    if (stage.loadLayout(*from, layout)) {
                        record = generatorRecord(layout, "SPECIAL");
                    } else {
                        unpacked = notUnpackedNote(play, from->name);
                    }
                }
            }
            const auto id = stage.generators().spawn(stage.device(), stage.enemies(), kind,
                                                     spawn->tier, spotAt(0), yaw, items, record);
            if (id) {
                stage.countSpawn();
                borrowVoices();
            }
            return afterNotes(
                std::array{voices, unpacked},
                id ? std::format("Stood a tier {} {} generator", spawn->tier, spawn->name)
                   : std::format("No {} generator could stand here", spawn->name));
        }
        case Kind::Item: {
            if (from) {
                return std::string(pickupWithMapRefusal(play));
            }
            const bool placed = stage.placeItem(spawn->name, spotAt(0));
            if (placed) {
                stage.countSpawn();
            }
            return placed ? std::format("Dropped {}", spawn->name)
                          : pickupMissingReply(play, spawn->name);
        }
        case Kind::Thing:
            if (!from && stage.placeItem(spawn->name, spotAt(0))) {
                stage.countSpawn();
                return std::format("Dropped {}", spawn->name);
            }
            [[fallthrough]];
        case Kind::Object: {
            const LevelRef source = from.value_or(stage.level());
            if (stage.spawnObject(source, spawn->name, spotAt(0), yaw)) {
                stage.countSpawn();
                return std::format("Stood {} of {}", spawn->name, source.name);
            }
            // A thing named without a map was looked for among the pickups as well.
            return spawn->kind == Kind::Thing && !from
                       ? thingMissingReply(play, spawn->name, source.name)
                       : objectMissingReply(play, spawn->name, source.name);
        }
        }
        return "Unknown spawn kind.";
    }
};

/** suicide: fells everyone standing. */
class SuicideVerb final : public WordlessVerb {
public:
    std::string_view name() const override { return "suicide"; }
    std::string_view summaryTail() const override { return "fells everyone standing"; }
    std::string apply(ConsoleStage& stage, s32 /*seat*/, Arguments arguments) const override {
        if (!arguments.empty()) {
            return "suicide takes no arguments.";
        }
        if (stage.isTower()) {
            return "Nobody falls in the tower.";
        }
        return fallenReply(stage.play(), stage.killEveryone());
    }
};

/** unlock_tower: opens the tower to everyone still in the party, and reopens it about them
 * there. */
class TowerUnlockVerb final : public WordlessVerb {
public:
    std::string_view name() const override { return "unlock_tower"; }
    std::string_view summaryTail() const override {
        return "opens every wing and portal of the tower";
    }
    std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const override {
        if (!arguments.empty()) {
            return "unlock_tower takes no arguments.";
        }
        const std::vector<CharacterSave*> saves = stage.presentSaves();
        for (CharacterSave* save : saves) {
            TowerAccess::openEverything(*save);
        }
        const auto opened = static_cast<s32>(saves.size());
        // Offline, with nobody still in, there is nobody to open the tower again for.
        if (opened == 0 && stage.play() == ConsolePlay::Offline) {
            return towerOpenedReply(stage.play(), opened);
        }
        if (stage.isTower()) {
            const PlayerActor* owner = stage.actor(seat);
            stage.requestTravel({LevelRef::tower(),
                                 owner ? std::optional{owner->position()} : std::nullopt,
                                 owner ? std::optional{owner->yaw()} : std::nullopt},
                                seat);
        }
        return towerOpenedReply(stage.play(), opened);
    }
};

} // namespace

const SharedConsoleVerb* sharedConsoleVerb(std::string_view lowered) {
    for (const SharedConsoleVerb* verb : sharedConsoleVerbs()) {
        if (verb->name() == lowered) {
            return verb;
        }
    }
    return nullptr;
}

std::span<const SharedConsoleVerb* const> sharedConsoleVerbs() {
    static const GotoVerb kGoto{};
    static const TowerUnlockVerb kTowerUnlock{};
    static const GiftVerb kGift{};
    static const SpawnVerb kSpawn{};
    static const SuicideVerb kSuicide{};
    static const CheatToggle kNoTarget{"notarget", &PlayScene::ConsoleCheats::noTarget,
                                       "the enemies stop seeing you, or see you again"};
    static const CheatToggle kGod{"god", &PlayScene::ConsoleCheats::god,
                                  "nothing harms you, or everything does again"};
    static const CheatToggle kNoclip{"noclip", &PlayScene::ConsoleCheats::noclip,
                                     "flies you through everything, or sets you down again"};
    static const std::array<const SharedConsoleVerb*, kSharedConsoleWords.size()> kVerbs{
        &kGoto, &kTowerUnlock, &kGift, &kSpawn, &kSuicide, &kNoTarget, &kGod, &kNoclip};
    return kVerbs;
}

bool consolePointInRange(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
           glm::all(glm::lessThanEqual(glm::abs(point), Vec3{kMaximumCoordinate}));
}

} // namespace gdl::game
