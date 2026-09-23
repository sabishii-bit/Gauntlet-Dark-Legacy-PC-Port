#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "game/screens/PlayScene.h"

namespace gdl::game {

/** One character of a scenario's party, named by the asset codes a save would carry. */
struct ScenarioMember {
    int player = 0;
    std::string classCode = "WAR";
    std::string colorCode = "YEL";
    std::string name = "TEST";
    int level = 1;
    std::vector<int> crystals; ///< per realm, as many as given
    int gold = 0;
    int health = 0; ///< none leaves it full
    int keys = 0;
    int slot = -1;            ///< the save slot the character is kept in; none when negative
    float turbo = 0.0f;       ///< what its turbo meter starts with
    std::vector<int> potions; ///< the kind of each carried
    std::vector<PowerupSlot> powerups; ///< carried and worn
    std::vector<int> legends;          ///< the legend items carried, by the realm of their boss
};

/**
 * A described start for testing: the party to bring into the tower, where they stand and
 * whether Sumner's welcome plays, read from a JSON file so a session can open straight onto
 * the moment under test instead of walking there through the title and select screens.
 */
struct Scenario {
    std::vector<ScenarioMember> party;
    PlayOptions tower;
    std::string level; ///< the level to open onto, such as G1; none is the tower

    /** Throws FormatError on malformed text, an unknown class or colour, or an empty party. */
    static Scenario fromJson(std::string_view text);
    /** Reads and parses a file; throws FileError or FormatError. */
    static Scenario load(const std::filesystem::path& file);

    /** The party as the select screen would hand it over. */
    std::vector<PartyMember> partyMembers() const;
};

} // namespace gdl::game
