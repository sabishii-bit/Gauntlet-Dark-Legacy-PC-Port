#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/screens/PlayScene.h"

namespace gdl::game {

/** One character of a scenario's party, named by the asset codes a save would carry. */
struct ScenarioMember {
    s32 player = 0;
    std::string classCode = "WAR";
    std::string colorCode = "YEL";
    std::string name = "TEST";
    s32 level = 1;
    s32 promotedLevel = -1;    ///< an earlier award stages a tower-return promotion
    std::vector<s32> crystals; ///< per realm, as many as given
    s32 gold = 0;
    s32 health = 0; ///< none leaves it full
    s32 keys = 0;
    s32 slot = -1;            ///< the save slot the character is kept in; none when negative
    f32 turbo = 0.0f;         ///< what its turbo meter starts with
    std::vector<s32> potions; ///< the kind of each carried
    std::vector<PowerupSlot> powerups; ///< carried and worn
    std::vector<s32> legends;          ///< the legend items carried, by the realm of their boss
    std::vector<s32> runes;            ///< installed runestones, indexed from zero
    std::vector<s32> shards;           ///< installed window pieces, tower realm order 1..8
    std::vector<s32> newRunes;         ///< awaiting the return ceremony
    std::vector<s32> newShards;
};

/**
 * A described start for testing: the party to bring into the tower, where they stand and
 * whether Sumner's welcome plays, read from a JSON file so a session can open straight onto
 * the moment under test instead of walking there through the title and select screens.
 */
struct Scenario {
    std::vector<ScenarioMember> party;
    PlayOptions tower;
    std::string level;       ///< the level to open onto, such as G1; none is the tower
    bool afterLevel = false; ///< tally/shop preview before returning to the tower
    std::vector<LevelResults> results;

    /** Throws FormatError on malformed text, an unknown class or colour, or an empty party. */
    static Scenario fromJson(std::string_view text);
    /** Reads and parses a file; throws FileError or FormatError. */
    static Scenario load(const std::filesystem::path& file);

    /** The party as the select screen would hand it over. */
    std::vector<PartyMember> partyMembers() const;
};

} // namespace gdl::game
