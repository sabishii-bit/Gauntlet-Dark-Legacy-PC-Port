#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

#include "engine/math/PortableMath.h"

#include "game/characters/Progression.h"
#include "game/console/ConsoleCommands.h"
#include "game/console/GiftRequest.h"
#include "game/console/NoclipMotion.h"
#include "game/console/SpawnRequest.h"
#include "game/level/TowerAccess.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {
namespace {
std::vector<std::string> wordsOf(std::string_view line) {
    std::vector<std::string> words;
    while (!line.empty()) {
        const usize first = line.find_first_not_of(' ');
        if (first == std::string_view::npos) {
            break;
        }
        line.remove_prefix(first);
        const usize end = line.find(' ');
        words.emplace_back(line.substr(0, end));
        line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
    }
    if (!words.empty()) {
        for (char& letter : words.front()) {
            if (letter >= 'A' && letter <= 'Z') {
                letter = static_cast<char>(letter - 'A' + 'a');
            }
        }
    }
    return words;
}

bool finite(const Vec3& point) {
    constexpr f32 kMaximumCoordinate = 1.0e6f;
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
           glm::all(glm::lessThanEqual(glm::abs(point), Vec3{kMaximumCoordinate}));
}
} // namespace

std::string PlayScene::applyConsoleAction(const ConsoleAction& action) {
    if (!action.valid() || !m_open || m_world == nullptr || m_device == nullptr) {
        return "No active scene for that console command.";
    }
    const auto words = wordsOf(action.line);
    const auto& verb = words.front();
    const auto arguments = std::span<const std::string>{words}.subspan(1);
    const s32 player = action.seat;
    CharacterSave* save = liveSave(player);
    if (save == nullptr) {
        return "That player is no longer in the party.";
    }
    if (m_sim.consoleTravel || m_sim.leaving) {
        return "The party is already on its way.";
    }
    if (verb == "god" || verb == "notarget" || verb == "noclip") {
        if (!arguments.empty()) {
            return "This command toggles without arguments.";
        }
        ConsoleCheats changed = cheats(player);
        bool& enabled = verb == "god"        ? changed.god
                        : verb == "notarget" ? changed.noTarget
                                             : changed.noclip;
        enabled = !enabled;
        setCheats(player, changed);
        return std::format("{} {}", verb, enabled ? "on" : "off");
    }
    if (verb == "get") {
        const auto gift = parseGift(arguments);
        if (!gift) {
            return gift.error();
        }
        constexpr s32 kMaximumGrant = 1000000;
        if (gift->amount > kMaximumGrant || gift->offer.amount > kMaximumGrant ||
            !std::isfinite(gift->offer.strength) || gift->offer.strength > kMaximumGrant) {
            return "A console grant must be finite and no larger than 1000000.";
        }
        switch (gift->kind) {
        case GiftRequest::Kind::Experience: {
            // PartyRecords applies native stage scaling and can share the award
            // with a combo partner. Check both additions before entering its s32
            // arithmetic; an untrusted console amount must never overflow it.
            const auto scaled = [&](s32 amount,
                                    const CharacterSave& character) -> std::optional<s32> {
                const f32 scale = m_world->level() != nullptr
                                      ? m_world->level()->tuning.experienceScale(
                                            experienceLevel(character.experience()))
                                      : 1.0f;
                const f32 won = static_cast<f32>(amount) * scale;
                if (!std::isfinite(won) || won < 0 ||
                    static_cast<f64>(won) >= std::numeric_limits<s32>::max()) {
                    return std::nullopt;
                }
                const auto points = static_cast<s32>(won);
                return static_cast<s64>(character.experience()) + points <=
                               std::numeric_limits<s32>::max()
                           ? std::optional{points}
                           : std::nullopt;
            };
            const auto awarded = scaled(gift->amount, *save);
            if (!awarded) {
                return "That experience grant would exceed the character's counter.";
            }
            for (const auto& runtime : m_players) {
                if (runtime.actor.player() != player || !runtime.combo.active()) {
                    continue;
                }
                const s32 partner = runtime.combo.partner;
                if (partner >= 0 && static_cast<usize>(partner) < m_players.size() &&
                    !scaled(*awarded, m_players[static_cast<usize>(partner)].actor.save())) {
                    return "That experience grant would exceed the partner's counter.";
                }
            }
            awardExperience(player, gift->amount, false);
            restat(player);
            return std::format("{} experience: level {}", gift->amount,
                               experienceLevel(save->experience()));
        }
        case GiftRequest::Kind::Level:
            save->progress().experience = levelExperience(gift->amount);
            restat(player);
            return std::format("Level {}", gift->amount);
        case GiftRequest::Kind::Item: {
            const auto& offer = gift->offer;
            const s64 amount = offer.amount;
            const s64 maximum = std::numeric_limits<s32>::max();
            if ((offer.kind == static_cast<s32>(ItemKind::Gold) &&
                 (static_cast<s64>(save->gold) + amount > maximum ||
                  static_cast<s64>(save->progress().lifetime.goldFound) + amount > maximum)) ||
                (offer.kind == static_cast<s32>(ItemKind::Food) &&
                 static_cast<s64>(save->health()) + amount > maximum)) {
                return "That item grant would exceed the character's counter.";
            }
            for (const auto& held : save->progress().inventory.powerups) {
                if (held.kind == offer.kind && held.flags == offer.flags &&
                    (!std::isfinite(held.charge + static_cast<f32>(offer.amount)) ||
                     !std::isfinite(held.strength + offer.strength * Inventory::kRenewShare) ||
                     held.charge + static_cast<f32>(offer.amount) > 10000000.0f ||
                     held.strength + offer.strength * Inventory::kRenewShare > 10000000.0f)) {
                    return "That item grant would exceed the powerup's counter.";
                }
            }
            return giftReport(*gift, takeItem(*save, gift->offer, 1.0f, true));
        }
        case GiftRequest::Kind::Info: return "get info is a local inspection command.";
        }
    }
    if (verb == "spawn") {
        return spawnConsoleAction(player, arguments);
    }
    if (verb == "suicide") {
        if (!arguments.empty()) {
            return "suicide takes no arguments.";
        }
        if (m_world->isTower()) {
            return "Nobody falls in the tower.";
        }
        s32 fallen = 0;
        for (s32 seat = 0; seat < kPlayerCount; ++seat) {
            fallen += kill(seat) ? 1 : 0;
        }
        return std::format("{} fallen", fallen);
    }
    if (verb == "goto") {
        const auto destination = ConsoleCommands::parseGoto(arguments);
        if (!destination) {
            return destination.error();
        }
        // Every peer refuses alike what the shared game cannot go to.
        if (destination->offlineOnly()) {
            return "Legends maps are offline only.";
        }
        if (destination->position && !finite(*destination->position)) {
            return "Coordinates must be finite and within +/-1000000.";
        }
        if (!destination->level) {
            if (!destination->position) {
                return ConsoleCommands::gotoUsage();
            }
            return relocate(*destination->position) ? "Moved the party." : "Nobody is standing.";
        }
        const auto level = *destination->level == "TOWER" ? std::optional{LevelRef::tower()}
                           : m_context.levels != nullptr
                               ? m_context.levels->byName(*destination->level)
                               : std::nullopt;
        if (!level) {
            return std::format("No map named {}", *destination->level);
        }
        // A native tick records intent only. The all-seat-confirmed terminal
        // frontier owns loading and replacement of the current scene.
        m_sim.consoleTravel = ConsoleTravelRequest{*level, destination->position, std::nullopt};
        m_sim.destination = *level;
        m_sim.secretTravel = level->isSecret();
        if (m_sim.secretTravel) {
            if (const auto* owner = actor(player)) {
                m_sim.secretReturnPosition = owner->position();
            }
        }
        return std::format("Going to {} ({})", level->name, level->title);
    }
    if (verb == "unlock_tower") {
        if (!arguments.empty()) {
            return "unlock_tower takes no arguments.";
        }
        for (auto& runtime : m_players) {
            if (!runtime.departed) {
                TowerAccess::openEverything(runtime.actor.save());
            }
        }
        if (m_world->isTower()) {
            const PlayerActor* owner = actor(player);
            m_sim.consoleTravel = ConsoleTravelRequest{
                LevelRef::tower(), owner ? std::optional{owner->position()} : std::nullopt,
                owner ? std::optional{owner->yaw()} : std::nullopt};
            m_sim.destination = LevelRef::tower();
            m_sim.secretTravel = false;
        }
        return "The tower stands open to the party.";
    }
    return "Unknown shared console command.";
}

std::string PlayScene::spawnConsoleAction(s32 player, std::span<const std::string> arguments) {
    const PlayerActor* owner = actor(player);
    if (owner == nullptr) {
        return "Nobody is in to spawn before.";
    }
    const auto spawn = parseSpawn(arguments);
    if (!spawn) {
        return spawn.error();
    }
    constexpr u32 kMaximumConsoleSpawns = 256;
    if (m_sim.consoleSpawns >= kMaximumConsoleSpawns) {
        return "The stage's console spawn limit is reached.";
    }
    std::optional<LevelRef> from;
    if (spawn->map) {
        from = *spawn->map == "TOWER"        ? std::optional{LevelRef::tower()}
               : m_context.levels != nullptr ? m_context.levels->byName(*spawn->map)
                                             : std::nullopt;
        if (!from) {
            return std::format("No map named {}", *spawn->map);
        }
    }
    const Vec3 facing = owner->facing();
    const f32 yaw = owner->yaw() + std::numbers::pi_v<f32>;
    const auto spotAt = [&](s32 index) {
        Vec3 spot = owner->position() + facing * (6.0f + 3.0f * static_cast<f32>(index));
        if (const auto floor = m_world->collision().floorAt(spot + Vec3{0, 3, 0}, 3, 10)) {
            spot.y = floor->y;
        }
        return spot;
    };
    const auto borrowVoices = [&] {
        if (!from) {
            return;
        }
        WorldData data;
        if (data.load(m_context.unpackedRoot / from->worldDataFile())) {
            if (const auto* level = data.level(from->name)) {
                borrowOpponentSounds(*level, data.audio(level->audioIndex));
            }
        }
    };
    using Kind = SpawnRequest::Kind;
    switch (spawn->kind) {
    case Kind::Enemy: {
        const auto views = m_opponents.views(m_players);
        s32 count = 0;
        for (s32 i = 0; i < spawn->count && m_sim.consoleSpawns < kMaximumConsoleSpawns; ++i) {
            if (enemies().loadKind(spawn->enemyKind) && enemies().spawn({.kind = spawn->enemyKind,
                                                                         .tier = spawn->tier,
                                                                         .position = spotAt(i),
                                                                         .direction = -facing,
                                                                         .placed = true},
                                                                        views)) {
                ++count;
                ++m_sim.consoleSpawns;
            }
        }
        if (count > 0) {
            borrowVoices();
        }
        return std::format("Spawned {} {} at tier {}", count, spawn->name, spawn->tier);
    }
    case Kind::Critter: {
        const char realm = from && !from->name.empty() ? from->name.front() : '\0';
        std::optional<s32> id;
        switch (spawn->critter) {
        case CombatantKind::Golem: id = critters().spawnGolem(spotAt(0), yaw, realm); break;
        case CombatantKind::General: id = critters().spawnGeneral(spotAt(0), yaw, 0, realm); break;
        case CombatantKind::Gargoyle:
            id = critters().spawnGargoyle(spotAt(0), yaw, spawn->form, realm);
            break;
        case CombatantKind::Unknown:
        case CombatantKind::Boss: break;
        }
        m_sim.consoleSpawns += id ? 1U : 0U;
        return id ? std::format("Spawned a {}", spawn->name)
                  : std::format("No room or artwork for {}", spawn->name);
    }
    case Kind::Generator: {
        s32 kind = spawn->enemyKind;
        ItemArchive* items = nullptr;
        WorldLayout layout;
        const ItemInfo* record = nullptr;
        if (spawn->brood) {
            const s32 realm = from ? from->realmId : m_world->ref().realmId;
            if (realm != TowerAccess::kTempleWorld && realm != TowerAccess::kUnderworldWorld) {
                return "A mixed brood needs a Temple (E) or Underworld (F) map.";
            }
            kind = realm == TowerAccess::kTempleWorld ? Generators::kTempleBrood
                                                      : Generators::kHellBrood;
            if (from) {
                items = borrowItems(*from);
                if (!items) {
                    return std::format("{} lends no item archive", from->name);
                }
                if (layout.load(m_context.unpackedRoot / from->directory)) {
                    record = generatorRecord(layout, "SPECIAL");
                }
            }
        }
        const auto id = generators().spawn(*m_device, enemies(), kind, spawn->tier, spotAt(0), yaw,
                                           items, record);
        m_sim.consoleSpawns += id ? 1U : 0U;
        if (id) {
            borrowVoices();
        }
        return id ? std::format("Stood a tier {} {} generator", spawn->tier, spawn->name)
                  : std::format("No {} generator could stand here", spawn->name);
    }
    case Kind::Item: {
        if (from) {
            return "Pickups come from the current level; omit the map.";
        }
        const bool placed = m_world->placeItem(*m_device, spawn->name, spotAt(0));
        m_sim.consoleSpawns += placed ? 1U : 0U;
        return placed ? std::format("Dropped {}", spawn->name)
                      : std::format("No pickup named {}", spawn->name);
    }
    case Kind::Thing:
        if (!from && m_world->placeItem(*m_device, spawn->name, spotAt(0))) {
            ++m_sim.consoleSpawns;
            return std::format("Dropped {}", spawn->name);
        }
        [[fallthrough]];
    case Kind::Object: {
        const LevelRef source = from.value_or(m_world->ref());
        const bool placed = spawnObject(source, spawn->name, spotAt(0), yaw);
        m_sim.consoleSpawns += placed ? 1U : 0U;
        return placed ? std::format("Stood {} of {}", spawn->name, source.name)
                      : std::format("No object named {} in {}", spawn->name, source.name);
    }
    }
    return "Unknown spawn kind.";
}

void PlayScene::advanceConsoleFlight(f64 deltaSeconds, const Inputs& inputs) {
    if (!m_open || !std::isfinite(deltaSeconds) || deltaSeconds <= 0) {
        return;
    }
    for (s32 player = 0; player < kPlayerCount; ++player) {
        if (!cheats(player).noclip) {
            continue;
        }
        Vec3 wish = inputs[static_cast<usize>(player)].consoleFlight;
        if (!finite(wish)) {
            continue;
        }
        const f32 length = glm::length(wish);
        if (length == 0) {
            continue;
        }
        if (length > 1) {
            wish /= length;
        }
        // World-space, quantized local intent crosses the input protocol, never
        // a local render delta, camera transform or already-integrated position.
        const Vec3 offset = wish * NoclipMotion::kSpeed * static_cast<f32>(deltaSeconds);
        const std::optional<f32> yaw = wish.x * wish.x + wish.z * wish.z > 1.0e-6f
                                           ? std::optional{portable::atan2(wish.x, wish.z)}
                                           : std::nullopt;
        noclipMove(player, offset, yaw);
    }
}

bool PlayScene::pressSwitch(usize index) {
    return m_open && m_world != nullptr && m_world->pressTrigger(index);
}

bool PlayScene::unlockGate(usize index) {
    return m_open && m_fixtures.unlockGate(index);
}

bool PlayScene::lockGate(usize index) {
    return m_open && m_fixtures.lockGate(index);
}

bool PlayScene::setChestLocked(usize index, bool locked) {
    return m_open && m_fixtures.setChestLocked(index, locked);
}

ScenePicking::Scene PlayScene::pickingScene() {
    return {m_world, m_opponents, m_fixtures, m_players, m_spawned};
}

std::optional<SceneSelection> PlayScene::pick(Vec2 cursor) {
    if (!m_open || m_world == nullptr || !m_presentedClip.has_value()) {
        return std::nullopt;
    }
    return ScenePicking::pick(pickingScene(), cursor, *m_presentedClip, m_presentedView,
                              m_presentedWorldAlpha);
}

void PlayScene::markSelection(const SelectionHighlight* highlight) {
    m_picking.markSelection(pickingScene(), highlight);
}

bool PlayScene::noclipMove(s32 player, const Vec3& offset, std::optional<f32> yaw) {
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() != player) {
            continue;
        }
        if (runtime.departed || runtime.life != PlayerLife::Standing) {
            return false;
        }
        runtime.actor.place(runtime.actor.position() + offset);
        if (yaw.has_value()) {
            runtime.actor.turnTo(*yaw);
        }
        runtime.fixtureSpot.reset();
        return true;
    }
    return false;
}

} // namespace gdl::game
