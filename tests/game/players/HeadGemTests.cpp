#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/HeadGem.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a hand of death or a health vampire sets its gem on the head, greeted once",
          "[game][players][powerups]") {
    constexpr u32 kBoth = powerup::kHandOfDeath | powerup::kHealthVamp;
    HeadGem gem;
    CHECK_FALSE(gem.update(0));
    CHECK(gem.shown().empty());
    CHECK(gem.update(kBoth)); // the burst plays as it appears; the hand of death first
    CHECK(gem.shown() == HeadGem::kHandOfDeath);
    CHECK_FALSE(gem.update(powerup::kHandOfDeath));
    // The hand gone and the vampire kept, the vampire's gem appears.
    CHECK(gem.update(powerup::kHealthVamp));
    CHECK(gem.shown() == HeadGem::kHealthVamp);
    CHECK_FALSE(gem.update(powerup::kHealthVamp));
    // Back to the hand: it was never forgotten (field_A1E stays set), so the vampire's gem
    // stays on with no burst, as the original leaves it.
    CHECK_FALSE(gem.update(kBoth));
    CHECK(gem.shown() == HeadGem::kHealthVamp);
    // With neither worn both are forgotten, and each greets again.
    CHECK_FALSE(gem.update(0));
    CHECK(gem.shown().empty());
    CHECK(gem.update(powerup::kHandOfDeath));
}

} // namespace
