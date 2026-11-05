#include <cmath>
#include <numbers>
#include <type_traits>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/Knockback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kFrame = 1.0f / 30.0f;
constexpr f32 kPace = 10.0f;

static_assert(std::is_trivially_copyable_v<Knockback>);

TEST_CASE("knockback's flag masks keep the original's DMG_ bits",
          "[game][players][knockback][flag-masks]") {
    STATIC_REQUIRE(Knockback::kKnockBack == 0x10U);
    STATIC_REQUIRE(Knockback::kKnockDown == 0x20U);
    STATIC_REQUIRE(Knockback::kBlownAway == 0x40U);
    STATIC_REQUIRE(Knockback::kKnockOver == 0x100U);
    STATIC_REQUIRE(Knockback::kWhirlwind == 0x10000U);
    STATIC_REQUIRE(Knockback::kFastSlide == 0x18160U);
}

TEST_CASE("contact adds direct velocity without requiring damage or a reaction",
          "[game][players][knockback][multiplayer]") {
    Knockback knock;
    knock.addVelocity({2, 0, 0});
    knock.addVelocity({0, 0, 4});
    CHECK_FALSE(knock.pending());
    CHECK_FALSE(knock.kick(0, false).has_value());
    const Vec3 step = knock.step(kFrame, kPace);
    CHECK(step.x == Approx(2 * kFrame));
    CHECK(step.z == Approx(4 * kFrame));
    CHECK(knock.velocity().x == Approx(2 * Knockback::kDecay));
    CHECK_FALSE(knock.pushed()); // distinct from the party shove/HUD state
    knock.clear();
    CHECK_FALSE(knock.sliding());
}

TEST_CASE("a knock back kicks sixteen times its push, a fall thirty-two, a blow away a hundred",
          "[game][players][knockback]") {
    const auto kicked = [](u32 flags, bool pojo = false) {
        Knockback knock;
        knock.queue(Vec3{0.0f, 0.0f, 1.0f}, flags, 10.0f);
        knock.kick(0.0f, pojo);
        return knock.velocity().z;
    };
    CHECK(kicked(Knockback::kKnockBack) == Approx(16.0f));
    CHECK(kicked(Knockback::kKnockDown) == Approx(32.0f));
    CHECK(kicked(Knockback::kKnockOver) == Approx(32.0f));
    CHECK(kicked(Knockback::kKnockDown, true) == Approx(80.0f)); // Pojo's
    CHECK(kicked(Knockback::kBlownAway) == Approx(100.0f));
    CHECK(kicked(Knockback::kWhirlwind) == Approx(100.0f));
    // The heaviest flag of the frame's hits decides.
    CHECK(kicked(Knockback::kKnockBack | Knockback::kKnockDown) == Approx(32.0f));
    // Nothing without a knock, or with a point or less of harm.
    CHECK(kicked(0x2000) == 0.0f);
    Knockback weak;
    weak.queue(Vec3{0.0f, 0.0f, 1.0f}, Knockback::kKnockBack, 1.0f);
    CHECK_FALSE(weak.kick(0.0f, false).has_value());
    CHECK(weak.velocity() == Vec3{0.0f});
}

TEST_CASE("a frame's pushes add up, never lift, and turn the body along or against them",
          "[game][players][knockback]") {
    Knockback knock;
    knock.queue(Vec3{1.0f, 0.0f, 0.0f}, Knockback::kKnockBack, 5.0f);
    knock.queue(Vec3{1.0f, 3.0f, 0.0f}, 0, 5.0f);
    REQUIRE(knock.pending());
    const auto heading = knock.kick(1.5f, false);
    CHECK_FALSE(knock.pending());
    CHECK(knock.velocity().x == Approx(32.0f)); // two pushes of one, at sixteen
    CHECK(knock.velocity().y == 0.0f);
    // Facing about along the push (a quarter turn off it), it turns to face along it.
    REQUIRE(heading.has_value());
    CHECK(*heading == Approx(std::numbers::pi_v<f32> / 2.0f));
    // Struck from in front it turns to face the blow.
    Knockback front;
    front.queue(Vec3{0.0f, 0.0f, -1.0f}, Knockback::kKnockBack, 5.0f);
    const auto facing = front.kick(0.0f, false);
    REQUIRE(facing.has_value());
    CHECK(std::abs(*facing) == Approx(0.0f).margin(1e-5f));
    Knockback behind;
    behind.queue(Vec3{0.0f, 0.0f, -1.0f}, Knockback::kKnockBack, 5.0f);
    const auto turned = behind.kick(std::numbers::pi_v<f32>, false);
    REQUIRE(turned.has_value());
    CHECK(std::abs(*turned) == Approx(std::numbers::pi_v<f32>));
    // A push straight up is no push.
    Knockback up;
    up.queue(Vec3{0.0f, 1.0f, 0.0f}, Knockback::kKnockDown, 10.0f);
    CHECK_FALSE(up.kick(0.0f, false).has_value());
    CHECK(up.velocity() == Vec3{0.0f});
}

TEST_CASE("a slide decays by a third a frame, held to the character's pace, and a fall's first "
          "frame travels freely",
          "[game][players][knockback]") {
    Knockback knock;
    knock.queue(Vec3{0.0f, 0.0f, 1.0f}, Knockback::kKnockBack, 10.0f);
    knock.kick(0.0f, false);
    // Sixteen a second would go 0.533 in a frame; the pace holds it to 1.5 x 10 / 30.
    const Vec3 first = knock.step(kFrame, kPace);
    CHECK(first.z == Approx(0.5f));
    CHECK(first.y == 0.0f);
    CHECK(knock.velocity().z == Approx(16.0f * Knockback::kDecay));
    const Vec3 second = knock.step(kFrame, kPace);
    CHECK(second.z == Approx(16.0f * Knockback::kDecay / 30.0f));
    // A knock down's kick goes its whole way the first frame, up to forty a second.
    Knockback floored;
    floored.queue(Vec3{0.0f, 0.0f, 1.0f}, Knockback::kKnockDown, 10.0f);
    floored.kick(0.0f, false);
    CHECK(floored.step(kFrame, kPace).z == Approx(32.0f / 30.0f));
    CHECK(floored.step(kFrame, kPace).z == Approx(0.5f)); // then the pace again
    Knockback blown;
    blown.queue(Vec3{0.0f, 0.0f, 1.0f}, Knockback::kBlownAway, 10.0f);
    blown.kick(0.0f, false);
    CHECK(blown.step(kFrame, kPace).z == Approx(40.0f / 30.0f));
    // The slide dies away, and the same over two half frames as one whole one.
    s32 frames = 0;
    while (knock.sliding() && frames < 100) {
        knock.step(kFrame, kPace);
        ++frames;
    }
    CHECK_FALSE(knock.sliding());
    CHECK(frames < 20);
    Knockback whole;
    Knockback halves;
    for (Knockback* k : {&whole, &halves}) {
        k->queue(Vec3{0.0f, 0.0f, 1.0f}, Knockback::kKnockBack, 10.0f);
        k->kick(0.0f, false);
        k->step(kFrame, 100.0f);
    }
    const f32 once = whole.step(kFrame, 100.0f).z;
    const f32 twice = halves.step(kFrame * 0.5f, 100.0f).z + halves.step(kFrame * 0.5f, 100.0f).z;
    CHECK(twice == Approx(once));
    CHECK(whole.velocity().z == Approx(halves.velocity().z));
    whole.clear();
    CHECK(whole.velocity() == Vec3{0.0f});
    CHECK(whole.step(kFrame, kPace) == Vec3{0.0f});
}
TEST_CASE("a shove carries the body at most half its pace and fades by a third a frame",
          "[game][players][knockback]") {
    Knockback knock;
    CHECK_FALSE(knock.pushed());
    knock.shove(Vec3{3.0f, 5.0f, 0.0f}, kFrame); // along the ground only
    knock.endFrame();
    CHECK(knock.pushed());
    // As the original adds it, a step of three a frame carries three a second.
    Vec3 moved = knock.step(kFrame, 10.0f);
    CHECK(moved.x == Approx(3.0f * kFrame));
    CHECK(moved.y == 0.0f);
    // At most half the pace: of ten, five.
    Knockback hard;
    hard.shove(Vec3{30.0f, 0.0f, 0.0f}, kFrame);
    CHECK(hard.step(kFrame, 10.0f).x == Approx(5.0f * kFrame));
    // A frame at 60 Hz shoves as much as its half of a 30 Hz frame's step.
    Knockback fine;
    fine.shove(Vec3{0.05f, 0.0f, 0.0f}, kFrame / 2.0f);
    moved = fine.step(kFrame, 100.0f);
    CHECK(moved.x == Approx(0.1f * kFrame));
    // The next frame's first shove starts afresh; unshoved, it is no longer pushed.
    knock.shove(Vec3{0.1f, 0.0f, 0.0f}, kFrame);
    moved = knock.step(kFrame, 10.0f);
    CHECK(moved.x == Approx(0.1f * kFrame));
    knock.endFrame();
    knock.endFrame();
    CHECK_FALSE(knock.pushed());
    for (s32 i = 0; i < 30; ++i) {
        knock.step(kFrame, 10.0f);
    }
    CHECK(knock.step(kFrame, 10.0f) == Vec3{0.0f});
}

TEST_CASE("knockback travel retains native frames across split update cadences",
          "[game][players][knockback][cadence]") {
    for (const u32 flags : {Knockback::kKnockBack, Knockback::kKnockDown, Knockback::kBlownAway,
                            Knockback::kWhirlwind}) {
        for (const f32 pace : {2.0f, 10.0f, 100.0f}) {
            CAPTURE(flags, pace);
            const auto travelAt = [&](s32 frequency) {
                Knockback knock;
                knock.queue(Vec3{1, 0, 1}, flags, 10);
                knock.kick(0, false);
                knock.shove(Vec3{1, 0, 0}, kFrame);
                Vec3 distance{0};
                for (s32 frame = 0; frame < frequency / 2; ++frame) {
                    distance += knock.step(1.0f / static_cast<f32>(frequency), pace);
                    // PartyMotion polls for a new kick after every motion update.
                    CHECK_FALSE(knock.kick(0, false).has_value());
                }
                return distance;
            };
            const Vec3 native = travelAt(30);
            for (const s32 frequency : {60, 120}) {
                CAPTURE(frequency);
                const Vec3 split = travelAt(frequency);
                CHECK(split.x == Approx(native.x));
                CHECK(split.z == Approx(native.z));
            }
        }
    }
}

TEST_CASE("fall kick keeps its free travel for the entire first native frame",
          "[game][players][knockback][cadence]") {
    Knockback knock;
    knock.queue(Vec3{0, 0, 1}, Knockback::kKnockDown, 10);
    knock.kick(0, false);
    const f32 first = knock.step(kFrame / 2, kPace).z;
    CHECK_FALSE(knock.kick(0, false).has_value());
    const f32 second = knock.step(kFrame / 2, kPace).z;
    CHECK(first == Approx(32 * kFrame / 2));
    CHECK(second == Approx(first));
    CHECK(knock.step(kFrame / 2, kPace).z == Approx(1.5f * kPace * kFrame / 2));
}

TEST_CASE("knockback copies retain pending hits and both generations of party shove",
          "[game][players][knockback][rollback]") {
    Knockback original;
    original.queue({1, 2, -3}, Knockback::kKnockDown, 5);
    REQUIRE(original.kick(0, false));
    original.step(kFrame / 4, kPace);
    original.shove({0.25f, 9, -0.5f}, kFrame / 2);
    original.endFrame();
    original.shove({-0.125f, -7, 0.75f}, kFrame / 2);
    original.queue({-2, 5, 1}, Knockback::kKnockBack, 4);
    original.queue({1, -2, 0}, 0x2000, 3);
    CHECK(original.pending());
    CHECK(original.velocity() == Vec3{32, 0, -96});
    CHECK(original.pushed());

    // The player's simulation holds its knockback by value: a copy is its rewind.
    Knockback restored = original;
    // Continuing a frame's shove must add to it, not replace it after a rewind.
    original.shove({0.1f, 0, -0.2f}, kFrame / 2);
    restored.shove({0.1f, 0, -0.2f}, kFrame / 2);
    CHECK(restored.step(kFrame / 2, kPace) == original.step(kFrame / 2, kPace));
    CHECK(restored.kick(-1, true) == original.kick(-1, true));
    CHECK(restored == original);
    original.endFrame();
    restored.endFrame();
    CHECK(restored == original);
    restored.clear();
    CHECK(restored == Knockback{});
}

TEST_CASE("knockback replay preserves kicks and decay at every split-frame boundary",
          "[game][players][knockback][rollback][cadence]") {
    for (const s32 frequency : {30, 60, 120, 144}) {
        CAPTURE(frequency);
        const f32 seconds = 1.0f / static_cast<f32>(frequency);
        const auto advance = [&](Knockback& knock, s32 tick) {
            // Motion consumes the existing slide before this invocation's pending kick.
            const Vec3 travel = knock.step(seconds, kPace);
            const auto heading = knock.kick(0.75f, tick % 2 == 0);
            if (tick % 3 == 0) {
                knock.shove({0.125f, 4, -0.25f}, seconds);
                knock.shove({-0.025f, 0, 0.125f}, seconds);
            }
            knock.endFrame();
            // Host combat follows movement and can queue multiple hits for the next tick.
            if (tick % 7 == 0) {
                knock.queue({1, 1, -1}, Knockback::kKnockDown, 5);
                knock.queue({-0.25f, -2, 0.5f}, Knockback::kKnockBack, 2);
            }
            if (tick % 11 == 0) {
                knock.addVelocity({0.5f, 0, -0.25f});
            }
            return std::pair{travel, heading};
        };
        Knockback original;
        for (s32 tick = 0; tick < 96; ++tick) {
            CAPTURE(tick);
            Knockback replayed = original;
            auto expected = original;
            for (s32 future = 0; future < 12; ++future) {
                CHECK(advance(replayed, tick + future) == advance(expected, tick + future));
                REQUIRE(replayed == expected);
            }
            advance(original, tick);
        }
    }
}

} // namespace
