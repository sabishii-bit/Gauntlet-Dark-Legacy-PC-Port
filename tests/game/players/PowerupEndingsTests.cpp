#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"
#include "game/players/PowerupEndings.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a special wearing off sounds its ending once, from the flags worn last frame",
          "[game][players][powerups][endings]") {
    const u32 all = powerup::kLevitation | powerup::kGrowth | powerup::kPojo;
    CHECK(PowerupEndings::soundsOf(0, 0, true).empty());
    CHECK(PowerupEndings::soundsOf(all, all, true).empty());
    CHECK(PowerupEndings::soundsOf(0, all, true).empty()); // coming on makes no ending
    CHECK(PowerupEndings::soundsOf(all, 0, true) ==
          std::vector<std::string_view>{"S_UNPOJO", "S_UNGROW", "S_LEVITATEDOWN"});
    CHECK(PowerupEndings::soundsOf(powerup::kLevitation, powerup::kGrowth, true) ==
          std::vector<std::string_view>{"S_LEVITATEDOWN"});
    CHECK(PowerupEndings::soundsOf(powerup::kPojo | powerup::kInvisible, powerup::kInvisible,
                                   true) == std::vector<std::string_view>{"S_UNPOJO"});
}

TEST_CASE("growth ending sounds only as the body goes back to its plain size",
          "[game][players][powerups][endings]") {
    CHECK(PowerupEndings::soundsOf(powerup::kGrowth, 0, true) ==
          std::vector<std::string_view>{"S_UNGROW"});
    CHECK(PowerupEndings::soundsOf(powerup::kGrowth, 0, false).empty()); // an ogre, a level 99
    CHECK(PowerupEndings::soundsOf(powerup::kGrowth | powerup::kLevitation, 0, false) ==
          std::vector<std::string_view>{"S_LEVITATEDOWN"});
}

} // namespace
