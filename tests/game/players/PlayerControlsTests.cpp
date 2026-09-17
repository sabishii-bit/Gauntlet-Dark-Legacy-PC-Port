#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"
#include "game/players/PlayerControls.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("keys walk along the axes and combine on the diagonal", "[game][players][controls]") {
    Input input;
    input.beginPoll();
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, true, kNoPad).any());
    input.setKey(Key::W, true);
    MoveInput move = readMoveInput(input, PlayBindings{}, true, kNoPad);
    REQUIRE(move.any());
    REQUIRE(move.direction == Vec2{0.0f, 1.0f});
    REQUIRE(move.magnitude == 1.0f);

    input.setKey(Key::D, true);
    move = readMoveInput(input, PlayBindings{}, true, kNoPad);
    REQUIRE(move.direction.x == Approx(0.7071f));
    REQUIRE(move.direction.y == Approx(0.7071f));
    REQUIRE(move.magnitude == 1.0f);

    // Opposite keys cancel; a player without the keyboard feels none of it.
    input.setKey(Key::S, true);
    REQUIRE(readMoveInput(input, PlayBindings{}, true, kNoPad).direction == Vec2{1.0f, 0.0f});
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, kNoPad).any());
}

TEST_CASE("the stick moves past its dead zone and the pad buttons add to it",
          "[game][players][controls]") {
    Input input;
    input.beginPoll();
    PadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.1f;
    input.setPad(1, pad);
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 1).any());

    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.0f;
    pad.axes[static_cast<usize>(PadAxis::LeftY)] = -1.0f; // the pad's up is negative
    input.setPad(1, pad);
    MoveInput move = readMoveInput(input, PlayBindings{}, false, 1);
    REQUIRE(move.direction == Vec2{0.0f, 1.0f});
    REQUIRE(move.magnitude == 1.0f);

    pad.axes[static_cast<usize>(PadAxis::LeftY)] = -0.625f;
    input.setPad(1, pad);
    move = readMoveInput(input, PlayBindings{}, false, 1);
    REQUIRE(move.magnitude == Approx(0.5f)); // half way through the live range

    pad.axes[static_cast<usize>(PadAxis::LeftY)] = 0.0f;
    pad.buttons[static_cast<usize>(PadButton::DpadRight)] = true;
    input.setPad(1, pad);
    move = readMoveInput(input, PlayBindings{}, false, 1);
    REQUIRE(move.direction == Vec2{1.0f, 0.0f});

    // Another pad, or none, does not see it; every pad does.
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 0).any());
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, kNoPad).any());
    REQUIRE(readMoveInput(input, PlayBindings{}, false, kAllPads).any());
}

} // namespace
