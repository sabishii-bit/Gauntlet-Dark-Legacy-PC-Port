#include "game/world/LevelCatalog.h"

#include <algorithm>
#include <array>
#include <exception>

#include "engine/assets/WorldData.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

namespace gdl::game {

namespace {

constexpr std::string_view kWorldDataDirectory = "wdata";
constexpr std::string_view kLevelsDirectory = "LEVELS";
constexpr std::string_view kItemsDirectory = "ITEMS";

std::string upper(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return out;
}

} // namespace

LevelRef LevelRef::tower() {
    return LevelRef{"TOWER",          kTowerRealm,    "L1",           "Tower",
                    "LEVELS/LEVELL1", "ITEMS/LEVELL", "ITEMS/LEVELL1"};
}

s32 LevelRef::orderOf(s32 realmId) {
    constexpr std::array<s32, 12> kOrder{kTowerRealm, 7, 2, 1, 11, 4, 3, 9, 10, 5, 6, 8};
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kOrder, realmId);
    return found != kOrder.end() ? static_cast<s32>(found - kOrder.begin()) : 0;
}

bool LevelCatalog::load(const std::filesystem::path& unpackedRoot) {
    m_realms.clear();
    const auto directory = AssetLocator(unpackedRoot)
                               .find(kWorldDataDirectory)
                               .value_or(unpackedRoot / kWorldDataDirectory);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        log::warn("Level catalogue: no realm data under {}", directory.string());
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const auto extension = toLowerAscii(entry.path().extension().string());
        if (extension != ".wad" && extension != ".json") {
            continue;
        }
        // A neighboring export must not duplicate (or replace a corrupt) native realm.
        if (extension == ".json" &&
            AssetLocator(directory).find(entry.path().stem().string() + ".wad")) {
            continue;
        }
        try {
            WorldData data;
            if (!data.load(entry.path())) {
                continue;
            }
            Realm realm;
            realm.file = upper(entry.path().stem().string());
            realm.id = static_cast<s32>(data.realm());
            realm.prefix = data.prefix();
            for (const auto& level : data.levels()) {
                realm.levels.push_back(upper(level.name));
                realm.titles.push_back(level.title);
                realm.runes.push_back(level.rune);
            }
            if (!realm.prefix.empty() && !realm.levels.empty()) {
                m_realms.push_back(std::move(realm));
            }
        } catch (const std::exception& e) {
            log::warn("Level catalogue: {}: {}", entry.path().string(), e.what());
        }
    }
    std::ranges::sort(m_realms, {}, &Realm::id);
    return !m_realms.empty();
}

LevelRef LevelCatalog::refOf(const Realm& realm, usize index) {
    LevelRef level;
    level.realm = realm.file;
    level.realmId = realm.id;
    level.name = realm.levels[index];
    level.title = realm.titles[index];
    const std::string prefix = upper(realm.prefix);
    // The folder is the realm's prefix less its letter, then the level's own name.
    level.directory =
        std::string(kLevelsDirectory) + "/" + prefix.substr(0, prefix.size() - 1) + level.name;
    level.items = std::string(kItemsDirectory) + "/" + prefix;
    // A boss level has an item archive of its own, named like its folder.
    level.ownItems =
        std::string(kItemsDirectory) + "/" + prefix.substr(0, prefix.size() - 1) + level.name;
    level.index = static_cast<s32>(index);
    return level;
}

std::vector<s32> LevelCatalog::runesOf(std::string_view realmFile) const {
    const std::string wanted = upper(realmFile);
    for (const Realm& realm : m_realms) {
        if (realm.file == wanted) {
            return realm.runes;
        }
    }
    return {};
}

usize LevelCatalog::levelCount(s32 realmId) const {
    for (const Realm& realm : m_realms) {
        if (realm.id == realmId) {
            return realm.levels.size();
        }
    }
    return 0;
}

bool LevelCatalog::isLastLevel(const LevelRef& level) const {
    const usize count = levelCount(level.realmId);
    return count > 0 && level.index == static_cast<s32>(count) - 1;
}

std::optional<LevelRef> LevelCatalog::byTag(std::string_view tag) const {
    if (tag.size() < 2 || tag[1] < '1' || tag[1] > '9') {
        return std::nullopt;
    }
    const char letter = upper(tag.substr(0, 1))[0];
    const auto index = static_cast<usize>(tag[1] - '1');
    for (const Realm& realm : m_realms) {
        if (upper(realm.prefix).back() == letter && index < realm.levels.size()) {
            return refOf(realm, index);
        }
    }
    return std::nullopt;
}

std::optional<LevelRef> LevelCatalog::byName(std::string_view name) const {
    const std::string wanted = upper(name);
    for (const Realm& realm : m_realms) {
        for (usize i = 0; i < realm.levels.size(); ++i) {
            if (realm.levels[i] == wanted) {
                return refOf(realm, i);
            }
        }
    }
    return std::nullopt;
}

bool LevelCatalog::unpacked(const std::filesystem::path& unpackedRoot, const LevelRef& level) {
    const AssetLocator files(unpackedRoot);
    return files.find(level.directory + "/worlds.ps2").has_value() ||
           files.find(level.directory + "/world.json").has_value();
}

} // namespace gdl::game
