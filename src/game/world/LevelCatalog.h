#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::game {

/** One level of the game, as the files name it. */
struct LevelRef {
    std::string realm;         ///< the realm's data file, such as TOWN
    std::int32_t realmId = -1; ///< the original's world id for the realm
    std::string name;          ///< the level's name in that file, such as G1
    std::string title;         ///< what the level is called on screen
    std::string directory;     ///< under the unpacked root, such as LEVELS/LEVELG1
    std::string items;         ///< the realm's item archive, such as ITEMS/LEVELG
    std::string ownItems;      ///< the level's own, such as ITEMS/LEVELG5, which boss levels have

    static constexpr std::int32_t kTowerRealm = 13;
    /** Sumner's tower, which needs no catalogue to be found. */
    static LevelRef tower();
    bool isTower() const { return realmId == kTowerRealm; }
    /** A realm's place in the tower's order (the tower nought, then the town, the mountain,
     * the castle, the sky, the forest, the desert, the ice, the dream, the temple, the
     * underworld and the battlefield), which the bosses' shards and the hints go by; nought
     * for a realm not in it. */
    static std::int32_t orderOf(std::int32_t realmId);
    /** The realm data file under the unpacked root. */
    std::string worldDataFile() const { return "wdata/" + realm + ".json"; }
    bool operator==(const LevelRef&) const = default;
};

/**
 * The game's realms and their levels, read from the unpacked realm data, and the way the
 * original finds a level from the two characters an exit portal carries: the letter names
 * the realm (the last of its level folders' prefix) and the digit counts into the realm's
 * list of levels, whose order is the realm's own (the castle's second is its A6).
 */
class LevelCatalog {
public:
    /** One realm's entry. */
    struct Realm {
        std::string file; ///< TOWN
        std::int32_t id = -1;
        std::string prefix;              ///< levelG
        std::vector<std::string> levels; ///< names in the realm's order
        std::vector<std::string> titles;
        std::vector<std::int32_t>
            runes; ///< the runestone each level holds, from one; nought for none
    };

    /** Reads every realm file in `wdata` under `unpackedRoot`; false when none could be. */
    bool load(const std::filesystem::path& unpackedRoot);
    bool loaded() const { return !m_realms.empty(); }
    const std::vector<Realm>& realms() const { return m_realms; }

    /** The level an exit's tag leads to ("g1"), or nothing for a tag that names none. */
    std::optional<LevelRef> byTag(std::string_view tag) const;
    /** The level of that name ("G1"), in any case. */
    std::optional<LevelRef> byName(std::string_view name) const;
    /** The runestones a realm's levels hold (the realm by its file, "TOWN"), from one. */
    std::vector<std::int32_t> runesOf(std::string_view realmFile) const;
    /** Whether the level's files are unpacked under `unpackedRoot`. */
    static bool unpacked(const std::filesystem::path& unpackedRoot, const LevelRef& level);

private:
    static LevelRef refOf(const Realm& realm, std::size_t index);

    std::vector<Realm> m_realms;
};

} // namespace gdl::game
