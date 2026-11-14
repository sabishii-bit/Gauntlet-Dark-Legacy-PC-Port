#include <map>
#include <set>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/PowerupFlags.h"
#include "game/level/CodePlacedSpecials.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the original's code stands 39 specials in 25 levels", "[code-specials]") {
    const auto specials = codePlacedSpecials();
    REQUIRE(specials.size() == 39);
    std::set<std::string_view> levels;
    std::map<std::string_view, s32> kinds;
    for (const CodePlacedSpecial& special : specials) {
        levels.insert(special.level);
        ++kinds[special.name];
        CAPTURE(special.level, special.name);
        const u32 flag = special.name == "MIKEYPUP"         ? powerup::kMikey
                         : special.name == "HEALTHVAMPPUP"  ? powerup::kHealthVamp
                         : special.name == "HANDOFDEATHPUP" ? powerup::kHandOfDeath
                                                            : 0U;
        CHECK(special.flag == flag);
    }
    CHECK(levels.size() == 25);
    CHECK(kinds["MIKEYPUP"] == 15);
    CHECK(kinds["HEALTHVAMPPUP"] == 13);
    CHECK(kinds["HANDOFDEATHPUP"] == 11);
    // The first row and the Ancient Tree's two Mikeys, as main.dol holds them.
    CHECK(specials[0].level == "G3");
    CHECK(specials[0].position == Vec3{99.9f, -20.0f, -293.6f});
    CHECK(specials[16].level == "D3");
    CHECK(specials[16].position == Vec3{-126.9f, 69.3f, -17.9f});
    CHECK(specials[17].level == "D3");
    CHECK(specials[17].position == Vec3{138.7f, 65.5f, 20.0f});
}

TEST_CASE("a level is a row's when the first four letters of its name begin the row's code",
          "[code-specials]") {
    CHECK(inLevel("D3", "D3"));
    CHECK_FALSE(inLevel("D3", "D4"));
    CHECK_FALSE(inLevel("C3", "D3"));
    CHECK(inLevel("D", "D3")); // a shorter name matches whatever follows
    CHECK_FALSE(inLevel("D31", "D3"));
    CHECK(inLevel("D3AB", "D3AB"));
    CHECK(inLevel("D3ABX", "D3AB")); // the level record keeps four letters
    CHECK_FALSE(inLevel("", "D3"));
    CHECK_FALSE(inLevel("n64:tower2", "D3"));
}

} // namespace
