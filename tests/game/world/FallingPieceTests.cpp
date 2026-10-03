#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/world/FallingPiece.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("falling profiles follow the item update's three rates", "[falling]") {
    const FallingProfile leaf = FallingProfile::of(FallingProfile::kLeafFall);
    REQUIRE(leaf.gravityStep == 1.0f);
    REQUIRE(leaf.spin == Approx(0.1745f));
    const FallingProfile sink = FallingProfile::of(FallingProfile::kRockSink);
    REQUIRE(sink.gravityStep == 2.0f);
    REQUIRE(sink.spin == Approx(0.01745f));
    for (const s32 subtype : {40, 51, 52}) {
        const FallingProfile rock = FallingProfile::of(subtype);
        REQUIRE(rock.gravityStep == 2.0f);
        REQUIRE(rock.spin == Approx(0.34906585f));
    }
}

TEST_CASE("frames come due at thirty a second with the rest carried", "[falling]") {
    f32 remainder = 0;
    REQUIRE(FallingPiece::framesDue(remainder, 0.05f) == 1);
    REQUIRE(remainder == Approx(0.05f - 1.0f / 30));
    REQUIRE(FallingPiece::framesDue(remainder, 0.0f) == 0);
    REQUIRE(FallingPiece::framesDue(remainder, 1.0f / 30 - remainder) == 1);
    REQUIRE(remainder == Approx(0.0f).margin(0.00001f));
    REQUIRE(FallingPiece::framesDue(remainder, 2.0f) == 60);
}

TEST_CASE("a piece drops by its profile's pull, spins by its instance and retires under the "
          "bottom",
          "[falling]") {
    FallingPiece piece;
    piece.place(Vec3{5, 10, -3}, Vec3{0, 1, 0}, 0);
    REQUIRE(piece.visible);
    REQUIRE(piece.velocity == Vec3{0});
    const FallingProfile rock{};
    piece.advance(rock, -100);
    REQUIRE(piece.velocity.y == -2.0f);
    REQUIRE(piece.position.x == 5.0f);
    REQUIRE(piece.position.z == -3.0f);
    REQUIRE(piece.position.y == Approx(10 - 2.0f / 30));
    // Instance 0 takes -4 from the table for its pitch and 4 (entry ~0 & 7) for its roll.
    REQUIRE(piece.rotation.x == Approx(-4 * 0.34906585f / 30));
    REQUIRE(piece.rotation.y == 1.0f);
    REQUIRE(piece.rotation.z == Approx(4 * 0.34906585f / 30));
    piece.advance(rock, -100);
    REQUIRE(piece.velocity.y == -4.0f);
    REQUIRE(piece.position.y == Approx(10 - 6.0f / 30));

    FallingPiece leaf;
    leaf.place(Vec3{0}, Vec3{0}, 3);
    leaf.advance(FallingProfile::of(FallingProfile::kLeafFall), -100);
    REQUIRE(leaf.velocity.y == -1.0f);
    REQUIRE(leaf.rotation.x == Approx(-1 * 0.1745f / 30));
    REQUIRE(leaf.rotation.z == Approx(1 * 0.1745f / 30));

    FallingPiece sinking;
    sinking.place(Vec3{0, -99, 0}, Vec3{0}, 5);
    sinking.advance(FallingProfile::of(FallingProfile::kRockSink), -99);
    REQUIRE(sinking.position.y == Approx(-99 - 2.0f / 30));
    REQUIRE_FALSE(sinking.visible);
    const Vec3 rest = sinking.position;
    sinking.advance(FallingProfile::of(FallingProfile::kRockSink), -99);
    REQUIRE(sinking.position == rest);
}

TEST_CASE("falling pieces retain authored physics at every caller cadence", "[falling][cadence]") {
    FallingPiece expected;
    expected.place(Vec3{3, 100, -2}, Vec3{0, 1, 0}, 5);
    for (s32 frame = 0; frame < 60; ++frame) {
        expected.advance(FallingProfile{}, -1000);
    }
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        FallingPiece piece;
        piece.place(Vec3{3, 100, -2}, Vec3{0, 1, 0}, 5);
        f32 remainder = 0;
        s32 ticks = 0;
        for (s32 update = 0; update < rate * 2; ++update) {
            const s32 due = FallingPiece::framesDue(remainder, 1.0f / static_cast<f32>(rate));
            ticks += due;
            for (s32 tick = 0; tick < due; ++tick) {
                piece.advance(FallingProfile{}, -1000);
            }
        }
        REQUIRE(ticks == 60);
        REQUIRE(piece.position == expected.position);
        REQUIRE(piece.rotation == expected.rotation);
        REQUIRE(piece.velocity == expected.velocity);
        REQUIRE(piece.visible == expected.visible);
    }
}
} // namespace
