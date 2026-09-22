#include "game/players/CharacterSave.h"

#include <algorithm>
#include <exception>
#include <format>
#include <system_error>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
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
        inventory.powerups[slot++] =
            PowerupSlot{entry.value("strength", 0.0f), entry.value("kind", 0),
                        entry.value("charge", 0.0f), entry.value("flags", 0U),
                        entry.value("on", true)};
    }
    return inventory;
}

Json relicsJson(const Relics& relics) {
    return Json{{"runes", relics.runes},
                {"legends", relics.legends},
                {"shards", relics.shards},
                {"gargoylePieces", relics.gargoylePieces}};
}

Relics relicsFromJson(const Json& object) {
    Relics relics;
    relics.runes = static_cast<u16>(object.value("runes", 0U));
    relics.legends = static_cast<u16>(object.value("legends", 0U));
    relics.shards = static_cast<u16>(object.value("shards", 0U));
    const auto pieces = object.value("gargoylePieces", std::vector<s32>{});
    for (usize kind = 0; kind < relics.gargoylePieces.size() && kind < pieces.size(); ++kind) {
        relics.gargoylePieces[kind] = pieces[kind];
    }
    return relics;
}

Json progressJson(const ClassProgress& progress) {
    return Json{{"experience", progress.experience}, {"health", progress.health},
                {"fightAdd", progress.fightAdd},     {"armorAdd", progress.armorAdd},
                {"magicAdd", progress.magicAdd},     {"speedAdd", progress.speedAdd},
                {"crystals", progress.crystals},     {"unlocked", progress.unlocked},
                {"inventory", inventoryJson(progress.inventory)},
                {"relics", relicsJson(progress.relics)}};
}

ClassProgress progressFromJson(const Json& object) {
    ClassProgress progress;
    progress.experience = object.value("experience", 0);
    progress.health = object.value("health", 0);
    progress.fightAdd = object.value("fightAdd", 0.0f);
    progress.armorAdd = object.value("armorAdd", 0.0f);
    progress.magicAdd = object.value("magicAdd", 0.0f);
    progress.speedAdd = object.value("speedAdd", 0.0f);
    progress.unlocked = object.value("unlocked", 0U);
    if (object.contains("inventory")) {
        progress.inventory = inventoryFromJson(object.at("inventory"));
    }
    if (object.contains("relics")) {
        progress.relics = relicsFromJson(object.at("relics"));
    }
    const auto crystals = object.value("crystals", std::vector<s32>{});
    for (usize realm = 0; realm < progress.crystals.size() && realm < crystals.size(); ++realm) {
        progress.crystals[realm] = crystals[realm];
    }
    return progress;
}

} // namespace

std::string CharacterSave::toJson() const {
    Json root;
    root["version"] = kSaveFormatVersion;
    root["name"] = name;
    root["character"] = character;
    root["color"] = color;
    root["classUnlock"] = classUnlock;
    root["gold"] = gold;
    root["helpSeen"] = helpSeen;
    root["levelTotal"] = levelTotal;
    Json progress = Json::object();
    for (s32 i = 0; i < kClassCount; ++i) {
        progress[std::string(classCode(i))] = progressJson(classes[static_cast<usize>(i)]);
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
    save.helpSeen = root.value("helpSeen", std::vector<s32>{});
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
        if (!std::filesystem::exists(file)) {
            continue;
        }
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
        writeTextFile(path(index), save.toJson());
    } catch (const std::exception& e) {
        log::warn("Saves: {}: {}", path(index).string(), e.what());
        return false;
    }
    SaveSlotInfo& info = m_slots[index];
    info.exists = true;
    info.name = save.name;
    info.character = save.character;
    info.color = save.color;
    return true;
}

} // namespace gdl::game
