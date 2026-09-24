#include "game/app/Scenario.h"

#include <algorithm>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "game/players/ClassData.h"
#include "game/players/Progression.h"

namespace gdl::game {

namespace {

using Json = nlohmann::json;

Vec3 readVec3(const Json& array) {
    if (!array.is_array() || array.size() != 3) {
        throw FormatError("scenario: a position needs three numbers");
    }
    return Vec3{array.at(0).get<f32>(), array.at(1).get<f32>(), array.at(2).get<f32>()};
}

} // namespace

Scenario Scenario::fromJson(std::string_view text) {
    Json root;
    try {
        root = Json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        throw FormatError(std::string("scenario: ") + e.what());
    }
    if (!root.is_object()) {
        throw FormatError("scenario: not an object");
    }
    const std::string screen = root.value("screen", std::string("tower"));
    if (screen != "tower" && screen != "shop") {
        throw FormatError("scenario: unknown screen " + screen);
    }
    Scenario scenario;
    scenario.afterLevel = screen == "shop";
    if (!root.contains("party") || !root.at("party").is_array() || root.at("party").empty()) {
        throw FormatError("scenario: the party is missing or empty");
    }
    for (const Json& entry : root.at("party")) {
        ScenarioMember member;
        member.player = entry.value("player", static_cast<s32>(scenario.party.size()));
        member.classCode = entry.value("class", member.classCode);
        member.colorCode = entry.value("color", member.colorCode);
        member.name = entry.value("name", member.name);
        member.level = entry.value("level", 1);
        member.promotedLevel = entry.value("promotedLevel", -1);
        member.crystals = entry.value("crystals", std::vector<s32>{});
        member.gold = entry.value("gold", 0);
        if (scenario.afterLevel) {
            const auto& result = entry.value("results", Json::object());
            scenario.results.push_back({member.player,
                                        {result.value("gold", 0), result.value("kills", 0),
                                         result.value("experience", 0)}});
        }
        member.health = entry.value("health", 0);
        member.keys = entry.value("keys", 0);
        member.slot = entry.value("slot", -1);
        member.turbo = entry.value("turbo", 0.0f);
        member.potions = entry.value("potions", std::vector<s32>{});
        member.legends = entry.value("legends", std::vector<s32>{});
        member.runes = entry.value("runes", std::vector<s32>{});
        member.shards = entry.value("shards", std::vector<s32>{});
        member.newRunes = entry.value("newRunes", std::vector<s32>{});
        member.newShards = entry.value("newShards", std::vector<s32>{});
        const auto invalidRune = [](s32 rune) { return rune < 0 || rune >= Relics::kRuneCount; };
        const auto invalidShard = [](s32 shard) { return shard < 1 || shard > 8; };
        if (std::ranges::any_of(member.runes, invalidRune) ||
            std::ranges::any_of(member.newRunes, invalidRune) ||
            std::ranges::any_of(member.shards, invalidShard) ||
            std::ranges::any_of(member.newShards, invalidShard)) {
            throw FormatError("scenario: a tower collectible is out of range");
        }
        for (const Json& powerup : entry.value("powerups", Json::array())) {
            member.powerups.push_back(
                PowerupSlot{powerup.value("strength", 30.0f), powerup.value("kind", 0),
                            powerup.value("charge", 0.0f), powerup.value("flags", 0U),
                            powerup.value("active", true)});
        }
        if (!classIndexOf(member.classCode).has_value()) {
            throw FormatError("scenario: unknown class " + member.classCode);
        }
        if (!colorIndexOf(member.colorCode).has_value()) {
            throw FormatError("scenario: unknown colour " + member.colorCode);
        }
        if (member.player < 0 || member.player >= PlayScene::kPlayerCount || member.name.empty() ||
            member.name.size() > kCharacterNameLength || member.level < 1 ||
            member.promotedLevel < -1 || member.promotedLevel == 0 ||
            member.promotedLevel > std::min(member.level, kMaxLevel) ||
            member.crystals.size() > kRealmCount || member.gold < 0 || member.health < 0 ||
            member.keys < 0 || member.keys > Inventory::kMostKeys ||
            member.potions.size() > static_cast<usize>(Inventory::kMostPotions) ||
            std::ranges::any_of(member.legends, [](s32 realm) {
                return realm < 1 || realm >= Relics::kRealmCount;
            })) {
            throw FormatError("scenario: a party member is out of range");
        }
        scenario.party.push_back(std::move(member));
    }
    if (root.contains("position")) {
        scenario.tower.position = readVec3(root.at("position"));
    }
    if (root.contains("yaw")) {
        scenario.tower.yaw = root.at("yaw").get<f32>();
    }
    if (root.contains("welcome")) {
        scenario.tower.welcome = root.at("welcome").get<bool>();
    }
    scenario.level = root.value("level", std::string{});
    scenario.tower.arrivalWorld = root.value("arrivalWorld", 0U);
    for (const Json& entry : root.value("items", Json::array())) {
        DroppedItem item;
        item.name = entry.value("name", std::string{});
        if (item.name.empty() || !entry.contains("position")) {
            throw FormatError("scenario: a dropped item needs a name and a position");
        }
        item.position = readVec3(entry.at("position"));
        scenario.tower.items.push_back(std::move(item));
    }
    return scenario;
}

Scenario Scenario::load(const std::filesystem::path& file) {
    return fromJson(readTextFile(file));
}

std::vector<PartyMember> Scenario::partyMembers() const {
    std::vector<PartyMember> members;
    for (const ScenarioMember& member : party) {
        CharacterSave save;
        save.name = member.name;
        save.character = classIndexOf(member.classCode).value_or(0);
        save.color = colorIndexOf(member.colorCode).value_or(0);
        ClassProgress& progress = save.progress();
        progress.experience = levelExperience(member.level);
        progress.promotedLevel = member.promotedLevel;
        for (usize realm = 0; realm < member.crystals.size(); ++realm) {
            progress.crystals[realm] = member.crystals[realm];
        }
        save.gold = member.gold;
        progress.health = member.health;
        progress.inventory.keys = member.keys;
        progress.inventory.potions = member.potions;
        for (const s32 realm : member.legends) {
            progress.relics.addLegend(realm);
        }
        for (const s32 rune : member.runes) {
            progress.relics.addRune(rune);
        }
        for (const s32 shard : member.shards) {
            progress.relics.addShard(shard);
        }
        progress.relics.pendingRunes = progress.relics.pendingShards = 0;
        for (const s32 rune : member.newRunes) {
            progress.relics.addRune(rune);
        }
        for (const s32 shard : member.newShards) {
            progress.relics.addShard(shard);
        }
        for (const PowerupSlot& slot : member.powerups) {
            progress.inventory.addPowerup(slot.kind, slot.flags, slot.charge, slot.strength);
            for (auto& held : progress.inventory.powerups) {
                if (held.kind == slot.kind && held.flags == slot.flags && held.held()) {
                    held.on = slot.on;
                }
            }
        }
        members.push_back(PartyMember{
            member.player, std::move(save),
            member.slot >= 0 ? std::optional<usize>{static_cast<usize>(member.slot)} : std::nullopt,
            false, member.turbo});
    }
    return members;
}

} // namespace gdl::game
