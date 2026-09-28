#include <string_view>

#include <catch2/catch_test_macros.hpp>


#include "game/players/ClassData.h"
#include "game/players/MagicPerks.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("potion magic learns its family's perk at 25 and the greater one at 50",
          "[game][players][magic-perks]") {
    CHECK_FALSE(MagicPerk::of(0, 24).has_value());
    const auto lesser = MagicPerk::of(0, 25);
    REQUIRE(lesser.has_value());
    CHECK(lesser->family == MagicPerkFamily::Treasure);
    CHECK_FALSE(lesser->greater);
    CHECK(MagicPerk::of(0, 50)->greater);
    CHECK(MagicPerk::of(0, 99)->greater);
    // The class table runs warrior, valkyrie, wizard, archer, and again for the rest.
    const auto family = [](std::string_view code) {
        return MagicPerk::of(*classIndexOf(code), 30)->family;
    };
    CHECK(family("VAL") == MagicPerkFamily::Traps);
    CHECK(family("WIZ") == MagicPerkFamily::Food);
    CHECK(family("ARC") == MagicPerkFamily::Walls);
    CHECK(family("DWF") == MagicPerkFamily::Treasure);
    CHECK(family("KNI") == MagicPerkFamily::Traps);
    CHECK(family("SOR") == MagicPerkFamily::Food);
    CHECK(family("JES") == MagicPerkFamily::Walls);
    CHECK(family("MIN") == MagicPerkFamily::Treasure);
    CHECK(family("FAL") == MagicPerkFamily::Traps);
    CHECK(family("JAC") == MagicPerkFamily::Food);
    CHECK(family("TIG") == MagicPerkFamily::Walls);
    CHECK(family("OGR") == MagicPerkFamily::Treasure);
    CHECK(family("UNI") == MagicPerkFamily::Traps);
    CHECK(family("MED") == MagicPerkFamily::Food);
    CHECK(family("HYE") == MagicPerkFamily::Walls);
    CHECK(family("SUM") == MagicPerkFamily::Food); // Sumner is made a wizard
    CHECK_FALSE(MagicPerk::of(kClassCount, 50).has_value());
}
