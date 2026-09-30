#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/app/MusicDuck.h"

namespace {

using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the music ducks a step a frame under the menu and climbs back released",
          "[music-duck][pause]") {
    // options.c 813 asks for a music scale of nought every frame the menu is up;
    // AudioMusicVolUpdate moves the volume 8 of 255 a frame towards it.
    MusicDuck duck;
    REQUIRE(duck.level() == 1.0f);
    duck.update(MusicDuck::kFrameSeconds, true);
    REQUIRE(duck.level() == Approx(1.0f - MusicDuck::kStep));
    // Half a frame steps nothing until the frame is whole.
    duck.update(MusicDuck::kFrameSeconds / 2, true);
    REQUIRE(duck.level() == Approx(1.0f - MusicDuck::kStep));
    duck.update(MusicDuck::kFrameSeconds / 2, true);
    REQUIRE(duck.level() == Approx(1.0f - 2 * MusicDuck::kStep));
    // Thirty-two frames reach silence; it stays there.
    duck.update(40 * MusicDuck::kFrameSeconds, true);
    REQUIRE(duck.level() == 0.0f);
    duck.update(1.0, true);
    REQUIRE(duck.level() == 0.0f);
    // Released (the Audio page, or the menu closed), it climbs back at the same rate.
    duck.update(MusicDuck::kFrameSeconds, false);
    REQUIRE(duck.level() == Approx(MusicDuck::kStep));
    duck.update(1.0, false);
    REQUIRE(duck.level() == Approx(31 * MusicDuck::kStep));
    duck.update(1.0, false);
    REQUIRE(duck.level() == 1.0f);
}

TEST_CASE("the music duck ignores bad clocks and steps at most a second at once",
          "[music-duck][pause]") {
    MusicDuck duck;
    duck.update(-1.0, true);
    duck.update(0.0, true);
    REQUIRE(duck.level() == 1.0f);
    duck.update(1e9, true);
    REQUIRE(duck.level() == Approx(1.0f - 30 * MusicDuck::kStep));
    duck.update(1e9, true);
    REQUIRE(duck.level() == 0.0f);
    duck.update(1e9, false);
    REQUIRE(duck.level() == Approx(30 * MusicDuck::kStep));
}

} // namespace
