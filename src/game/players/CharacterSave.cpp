#include "game/players/CharacterSave.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/Assert.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl::game {

namespace {

using Json = nlohmann::json;

constexpr s32 kSaveFormatVersion = 1;

/** What is carried; only the powerup slots that hold something are written. */
Json inventoryJson(const Inventory& inventory) {
    Json powerups = Json::array();
    for (const PowerupSlot& slot : inventory.powerups) {
        if (slot.held()) {
            powerups.push_back(Json{{"kind", slot.kind},
                                    {"flags", slot.flags},
                                    {"strength", slot.strength},
                                    {"charge", slot.charge},
                                    {"on", slot.on}});
        }
    }
    return Json{{"keys", inventory.keys}, {"potions", inventory.potions}, {"powerups", powerups}};
}

Inventory inventoryFromJson(const Json& object) {
    Inventory inventory;
    inventory.keys = std::clamp(object.value("keys", 0), 0, Inventory::kMostKeys);
    inventory.potions = object.value("potions", std::vector<s32>{});
    inventory.potions.resize(
        std::min(inventory.potions.size(), static_cast<usize>(Inventory::kMostPotions)));
    usize slot = 0;
    for (const Json& entry : object.value("powerups", Json::array())) {
        if (slot >= inventory.powerups.size()) {
            break;
        }
        inventory.powerups[slot++] = PowerupSlot{
            entry.value("strength", 0.0f), entry.value("kind", 0), entry.value("charge", 0.0f),
            entry.value("flags", 0U), entry.value("on", true)};
    }
    return inventory;
}

Json relicsJson(const Relics& relics) {
    return Json{{"runes", relics.runes},
                {"legends", relics.legends},
                {"shards", relics.shards},
                {"pendingRunes", relics.pendingRunes},
                {"pendingShards", relics.pendingShards},
                {"pendingCeremonies", relics.pendingCeremonies},
                {"gargoylePieces", relics.gargoylePieces}};
}

Relics relicsFromJson(const Json& object) {
    Relics relics;
    relics.runes = static_cast<u16>(object.value("runes", 0U));
    relics.legends = static_cast<u16>(object.value("legends", 0U));
    relics.shards = static_cast<u16>(object.value("shards", 0U));
    // Older saves already banked their collection. Only explicit pending bits
    // replay a ceremony, including a return interrupted by saving and quitting.
    relics.pendingRunes = static_cast<u16>(object.value("pendingRunes", 0U) & relics.runes);
    relics.pendingShards = static_cast<u16>(object.value("pendingShards", 0U) & relics.shards);
    relics.pendingCeremonies =
        static_cast<u16>(object.value("pendingCeremonies", 0U) & Relics::kTowerCeremonyMask);
    const auto pieces = object.value("gargoylePieces", std::vector<s32>{});
    for (usize kind = 0; kind < relics.gargoylePieces.size() && kind < pieces.size(); ++kind) {
        relics.gargoylePieces[kind] = pieces[kind];
    }
    return relics;
}

Json levelsJson(const LevelRecord& levels) {
    return Json{{"beaten", levels.beaten},
                {"runeLevels", levels.runeLevels},
                {"legendLevels", levels.legendLevels},
                {"bossDeaths", levels.bossDeaths}};
}

/** Reads a pair of pass masks: the first time, and the second. */
std::array<u16, LevelRecord::kPasses> passesFromJson(const Json& object, std::string_view key) {
    std::array<u16, LevelRecord::kPasses> passes{};
    const auto values = object.value(key, std::vector<u32>{});
    for (usize i = 0; i < passes.size() && i < values.size(); ++i) {
        passes[i] = static_cast<u16>(values[i]);
    }
    return passes;
}

/** A save from before the record loads as nothing beaten and no boss died on. */
LevelRecord levelsFromJson(const Json& object) {
    LevelRecord levels;
    const auto beaten = object.value("beaten", std::vector<u32>{});
    for (usize realm = 0; realm < levels.beaten.size() && realm < beaten.size(); ++realm) {
        levels.beaten[realm] = static_cast<u8>(beaten[realm]);
    }
    levels.runeLevels = passesFromJson(object, "runeLevels");
    levels.legendLevels = passesFromJson(object, "legendLevels");
    levels.bossDeaths = passesFromJson(object, "bossDeaths");
    return levels;
}

Json progressJson(const ClassProgress& progress) {
    return Json{{"experience", progress.experience},
                {"gold", progress.gold},
                {"promotedLevel", progress.appearanceLevel()},
                {"health", progress.health},
                {"fightAdd", progress.fightAdd},
                {"armorAdd", progress.armorAdd},
                {"magicAdd", progress.magicAdd},
                {"speedAdd", progress.speedAdd},
                {"crystals", progress.crystals},
                {"unlocked", progress.unlocked},
                {"lifetime",
                 {{"enemiesKilled", progress.lifetime.enemiesKilled},
                  {"generatorsDestroyed", progress.lifetime.generatorsDestroyed},
                  {"goldFound", progress.lifetime.goldFound},
                  {"playSeconds", progress.lifetime.playSeconds}}},
                {"inventory", inventoryJson(progress.inventory)},
                {"relics", relicsJson(progress.relics)},
                {"levels", levelsJson(progress.levels)}};
}

ClassProgress progressFromJson(const Json& object) {
    ClassProgress progress;
    progress.experience = object.value("experience", 0);
    progress.gold = object.value("gold", 0);
    progress.promotedLevel =
        std::clamp(object.value("promotedLevel", experienceLevel(progress.experience)), 1,
                   experienceLevel(progress.experience));
    progress.health = object.value("health", 0);
    progress.fightAdd = object.value("fightAdd", 0.0f);
    progress.armorAdd = object.value("armorAdd", 0.0f);
    progress.magicAdd = object.value("magicAdd", 0.0f);
    progress.speedAdd = object.value("speedAdd", 0.0f);
    progress.unlocked = object.value("unlocked", 0U);
    const auto totals = object.value("lifetime", Json::object());
    progress.lifetime = {std::max(0, totals.value("enemiesKilled", 0)),
                         std::max(0, totals.value("generatorsDestroyed", 0)),
                         std::max(0, totals.value("goldFound", 0)),
                         std::max(0.0, totals.value("playSeconds", 0.0))};
    if (!std::isfinite(progress.lifetime.playSeconds) || progress.lifetime.playSeconds > 1e12) {
        throw FormatError("character save: invalid lifetime playtime");
    }
    if (object.contains("inventory")) {
        progress.inventory = inventoryFromJson(object.at("inventory"));
    }
    if (object.contains("relics")) {
        progress.relics = relicsFromJson(object.at("relics"));
    }
    if (object.contains("levels")) {
        progress.levels = levelsFromJson(object.at("levels"));
    }
    const auto crystals = object.value("crystals", std::vector<s32>{});
    for (usize realm = 0; realm < progress.crystals.size() && realm < crystals.size(); ++realm) {
        progress.crystals[realm] = crystals[realm];
    }
    return progress;
}

} // namespace

void CharacterSave::selectClass(s32 next) {
    GDL_VERIFY(next >= 0 && next < kClassCount, "Selected class must be in range");
    if (next == character) {
        return;
    }
    progress().gold = gold;
    character = next;
    gold = progress().gold;
}

std::string CharacterSave::toJson() const {
    Json root;
    root["version"] = kSaveFormatVersion;
    root["name"] = name;
    root["character"] = character;
    root["color"] = color;
    root["classUnlock"] = classUnlock;
    root["gold"] = gold;
    root["autoAttack"] = autoAttack;
    root["helpSeen"] = helpSeen;
    root["moviesSeen"] = moviesSeen;
    root["levelTotal"] = levelTotal;
    Json progress = Json::object();
    for (s32 i = 0; i < kClassCount; ++i) {
        Json entry = progressJson(classes[static_cast<usize>(i)]);
        if (i == character) {
            entry["gold"] = gold;
        }
        progress[std::string(classCode(i))] = std::move(entry);
    }
    root["classes"] = progress;
    return root.dump(2) + "\n";
}

CharacterSave CharacterSave::fromJson(std::string_view text) {
    Json root;
    try {
        root = Json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        throw FormatError(std::string("character save: ") + e.what());
    }
    if (!root.is_object() || !root.contains("name") || !root.contains("character")) {
        throw FormatError("character save: not a character");
    }
    CharacterSave save;
    save.name = root.at("name").get<std::string>();
    save.character = root.at("character").get<s32>();
    save.color = root.value("color", 0);
    save.classUnlock = static_cast<u16>(root.value("classUnlock", 0));
    save.gold = root.value("gold", 0);
    save.autoAttack = root.value("autoAttack", save.autoAttack);
    save.helpSeen = root.value("helpSeen", std::vector<s32>{});
    save.moviesSeen = root.value("moviesSeen", std::vector<std::string>{});
    std::ranges::sort(save.helpSeen);
    save.levelTotal = root.value("levelTotal", 0);
    if (save.character < 0 || save.character >= kClassCount || save.color < 0 ||
        save.color >= kColorCount || save.name.size() > kCharacterNameLength) {
        throw FormatError("character save: values out of range");
    }
    if (root.contains("classes")) {
        const Json& progress = root.at("classes");
        for (s32 i = 0; i < kClassCount; ++i) {
            const std::string code(classCode(i));
            if (progress.contains(code)) {
                save.classes[static_cast<usize>(i)] = progressFromJson(progress.at(code));
            }
        }
    }
    // The top-level wallet remains authoritative for the selected class, including
    // legacy saves that never recorded balances for the other classes.
    save.progress().gold = save.gold;
    return save;
}

bool SaveSlots::open(const std::filesystem::path& directory, usize count) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        log::warn("Saves: cannot create {}: {}", directory.string(), error.message());
        m_directory.clear();
        m_slots.clear();
        return false;
    }
    m_directory = directory;
    m_slots.assign(count, SaveSlotInfo{});
    refresh();
    return true;
}

std::filesystem::path SaveSlots::path(usize index) const {
    return m_directory / std::format("slot{}.json", index + 1);
}

void SaveSlots::refresh() {
    for (usize i = 0; i < m_slots.size(); ++i) {
        SaveSlotInfo& info = m_slots[i];
        info = SaveSlotInfo{};
        const std::filesystem::path file = path(i);
        std::error_code error;
        const bool exists = std::filesystem::exists(file, error);
        if (!exists && !error) {
            continue;
        }
        info.occupied = true;
        try {
            const CharacterSave save = CharacterSave::fromJson(readTextFile(file));
            info.exists = true;
            info.name = save.name;
            info.character = save.character;
            info.color = save.color;
        } catch (const std::exception& e) {
            log::warn("Saves: {}: {}", file.string(), e.what());
        }
    }
}

bool SaveSlots::anySaved() const {
    return std::ranges::any_of(m_slots, [](const SaveSlotInfo& info) { return info.exists; });
}

bool SaveSlots::load(usize index, CharacterSave& out) const {
    if (index >= m_slots.size() || !m_slots[index].exists) {
        return false;
    }
    try {
        out = CharacterSave::fromJson(readTextFile(path(index)));
        return true;
    } catch (const std::exception& e) {
        log::warn("Saves: {}: {}", path(index).string(), e.what());
        return false;
    }
}

bool SaveSlots::write(usize index, const CharacterSave& save) {
    if (index >= m_slots.size()) {
        return false;
    }
    try {
        replaceTextFile(path(index), save.toJson());
    } catch (const std::exception& e) {
        log::warn("Saves: {}: {}", path(index).string(), e.what());
        return false;
    }
    SaveSlotInfo& info = m_slots[index];
    info.exists = true;
    info.occupied = true;
    info.name = save.name;
    info.character = save.character;
    info.color = save.color;
    return true;
}

} // namespace gdl::game
