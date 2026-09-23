
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/app/AttractSequencer.h"

namespace {

using namespace gdl::game;

TEST_CASE("the attract loop opens with the three intro movies", "[game][attract]") {
    AttractSequencer sequencer;
    const AttractStep first = sequencer.next();
    REQUIRE(first.screen == AttractScreen::TitleMovie);
    REQUIRE(first.movie == "midway");
    const AttractStep second = sequencer.next();
    REQUIRE(second.screen == AttractScreen::Movie);
    REQUIRE(second.movie == "opening");
    const AttractStep third = sequencer.next();
    REQUIRE(third.movie == "title2");
    const AttractStep fourth = sequencer.next();
    REQUIRE(fourth.screen == AttractScreen::Screen2D);
    REQUIRE(fourth.movie.empty());
    REQUIRE(sequencer.next().screen == AttractScreen::TitleScreen);
}

TEST_CASE("the character movie row follows the wave", "[game][attract]") {
    REQUIRE(AttractSequencer::movieName(3, 0) == "war_dwa");
    REQUIRE(AttractSequencer::movieName(3, 1) == "val_kni");
    REQUIRE(AttractSequencer::movieName(3, 2) == "wiz_sor");
    REQUIRE(AttractSequencer::movieName(3, 3) == "jes_arc");
    REQUIRE(AttractSequencer::movieName(4, 2) == "fail");
    REQUIRE(AttractSequencer::movieName(9, 0).empty());
}

TEST_CASE("wrapping the table advances the wave and reset returns to the start",
          "[game][attract]") {
    AttractSequencer sequencer;
    for (gdl::usize i = 0; i < AttractSequencer::kScreenTable.size(); ++i) {
        sequencer.next();
    }
    REQUIRE(sequencer.position() == 0);
    REQUIRE(sequencer.wave() == 1);
    for (gdl::usize i = 0; i < 7; ++i) {
        sequencer.next();
    }
    REQUIRE(sequencer.next().movie == "val_kni");
    sequencer.reset();
    REQUIRE(sequencer.wave() == 0);
    REQUIRE(sequencer.next().movie == "midway");
}

} // namespace
