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
    std::vector<s32> crystals; ///< per realm, as many as given
    s32 gold = 0;
    s32 health = 0; ///< none leaves it full
    s32 keys = 0;
    s32 slot = -1; ///< the save slot the character is kept in; none when negative
    f32 turbo = 0.0f; ///< what its turbo meter starts with
    std::vector<s32> potions; ///< the kind of each carried
    std::vector<PowerupSlot> powerups; ///< carried and worn
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
