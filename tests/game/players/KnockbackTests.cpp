#include <cmath>
#include <numbers>

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
    CHECK(twice == Approx(once).epsilon(0.1f));
    CHECK(whole.velocity().z == Approx(halves.velocity().z));
    whole.clear();
    CHECK(whole.velocity() == Vec3{0.0f});
    CHECK(whole.step(kFrame, kPace) == Vec3{0.0f});
}
} // namespace
