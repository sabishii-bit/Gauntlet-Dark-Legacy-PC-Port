#include <array>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsSwitches.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
formats::LegendsN64Switch lift(u32 index = 0) {
    formats::LegendsN64Switch result;
    result.nativeIndex = index;
    result.templateKey = 0x1202;
    result.flags = 0x3802;
    result.activationFlags = 0x02000000;
    result.target = "ELEV1";
    result.firstEndpoint = -100;
    result.secondEndpoint = 0;
    result.supported = true;
    return result;
}
} // namespace

TEST_CASE("Native lift initializes lowered and moves once to its authored endpoint",
          "[legends][switches]") {
    const std::array definitions{lift()};
    LegendsSwitches motion{definitions};
    REQUIRE(motion.targets().size() == 1);
    CHECK(motion.targets()[0].offset == -10);
    CHECK_FALSE(motion.targets()[0].activated);
    motion.update(1);
    CHECK(motion.targets()[0].offset == -10);
    CHECK_FALSE(motion.activate(7));
    REQUIRE(motion.activate(0));
    CHECK_FALSE(motion.activate(0));
    motion.capturePresentation();
    motion.update(1);
    CHECK(motion.targets()[0].offset == -6);
    CHECK(motion.targets()[0].sample(0) == -10);
    CHECK(motion.targets()[0].sample(.5f) == -8);
    CHECK(motion.targets()[0].sample(1) == -6);
    CHECK(motion.targets()[0].moving);
    motion.update(2);
    CHECK(motion.targets()[0].offset == 0);
    CHECK_FALSE(motion.targets()[0].moving);
    motion.update(10);
    CHECK(motion.targets()[0].offset == 0);
    CHECK(motion.switches()[0].activated);
}

TEST_CASE("Native fading bridge combines pads and retains supported actors without displacement",
          "[legends][switches][native-pressure-bridge]") {
    auto first = lift();
    first.flags = 0x2102;
    auto second = first;
    second.nativeIndex = 1;
    LegendsSwitches motion{std::array{first, second}};
    REQUIRE(motion.targets().size() == 1);
    CHECK(motion.targets()[0].fading);
    CHECK(motion.targets()[0].offset == 1.5f);
    CHECK(motion.targets()[0].first == 1.5f);
    CHECK(motion.targets()[0].second == 0);
    motion.contact(0, 1);
    motion.contact(1, 0);
    motion.update(.1875f);
    CHECK(motion.targets()[0].offset == .75f);
    motion.contact(0, 1);
    motion.contact(1, 0);
    motion.update(.1875f);
    CHECK(motion.targets()[0].offset == 0);
    motion.contact(0, 0);
    motion.contact(1, 0);
    motion.support(std::array<usize, 1>{0});
    motion.update(1);
    motion.update(1);
    CHECK(motion.targets()[0].offset == 0);
    motion.support({});
    motion.update(.1875f);
    CHECK(motion.targets()[0].offset == .75f);
    // The other pad reverses a partially completed return; it does not create
    // a second target or wait for the bridge to vanish first.
    motion.contact(1, 2);
    motion.update(.1875f);
    CHECK(motion.targets()[0].offset == 0);
    motion.contact(1, 0);
    motion.update(.01f); // Native active->idle state sends one final request.
    motion.update(.375f);
    CHECK(motion.targets()[0].offset == 1.5f);
    CHECK_FALSE(motion.targets()[0].moving);
}

TEST_CASE("Native breakaway descends without returning and shares target initialization",
          "[legends][switches]") {
    auto plate = lift();
    plate.flags = 0x3502;
    plate.firstEndpoint = 0;
    plate.secondEndpoint = -60;
    const std::array definitions{plate};
    LegendsSwitches motion{definitions};
    REQUIRE(motion.activate(0));
    motion.update(.5f);
    CHECK(motion.targets()[0].offset == -2);
    motion.update(2);
    CHECK(motion.targets()[0].offset == -6);
    CHECK_FALSE(motion.targets()[0].moving);
    plate.secondEndpoint = 0;
    const LegendsSwitches defaultDrop{std::array{plate}};
    CHECK(defaultDrop.targets()[0].second == -50);

    auto second = plate;
    second.nativeIndex = 1;
    second.firstEndpoint = -40;
    second.secondEndpoint = 25;
    LegendsSwitches shared{std::array{plate, second}};
    REQUIRE(shared.targets().size() == 1);
    REQUIRE(shared.switches().size() == 2);
    CHECK(shared.targets()[0].first == -4);
    CHECK(shared.targets()[0].offset == -4);
    CHECK(shared.targets()[0].second == -50); // Nonzero native endpoints are not replaced.
    CHECK(shared.activate(1));
    CHECK(shared.activate(0));
    shared.update(1);
    CHECK(shared.targets()[0].offset == -8);
}

TEST_CASE("Native shared fade records retain the existing-record endpoint fill rule",
          "[legends][switches][native-pressure-bridge]") {
    auto first = lift();
    first.flags = 0x2102;
    first.firstEndpoint = 20;
    first.secondEndpoint = 30;
    auto second = first;
    second.nativeIndex = 1;
    second.firstEndpoint = 40;
    second.secondEndpoint = 5;
    auto third = second;
    third.nativeIndex = 2;
    third.secondEndpoint = 10;

    // e006dea0 replaces the first placement's parameters with 1.5/0 for a
    // new fade record. Subsequent placements fill zero endpoints before the
    // mode dispatch, so the first nonzero second parameter survives. All 24
    // installed fade pads use 0/0; this pins the native constructor edge case.
    LegendsSwitches motion{std::array{first, second, third}};
    REQUIRE(motion.targets().size() == 1);
    CHECK(motion.targets()[0].fading);
    CHECK(motion.targets()[0].first == 1.5f);
    CHECK(motion.targets()[0].second == .5f);
    CHECK(motion.targets()[0].offset == 1.5f);
    motion.contact(1, 1);
    motion.update(.25f);
    CHECK(motion.targets()[0].offset == .5f);
}

TEST_CASE("Native switch contact does not admit dead players or unsupported item triggers",
          "[legends][switches]") {
    auto plate = lift();
    plate.position = {3, 2, 7};
    LegendsPortalVisitor visitor{0, {3, 2, 7}, true, .75f, 3};
    CHECK(touchesLegendsSwitch(plate, visitor));
    visitor.position.x += plate.radius + visitor.radius;
    CHECK_FALSE(touchesLegendsSwitch(plate, visitor));
    visitor.position = {3, 3.001f, 7};
    CHECK_FALSE(touchesLegendsSwitch(plate, visitor));
    visitor.position = plate.position;
    visitor.alive = false;
    CHECK_FALSE(touchesLegendsSwitch(plate, visitor));
    visitor.alive = true;
    plate.supported = false;
    CHECK_FALSE(touchesLegendsSwitch(plate, visitor));
    CHECK(LegendsSwitches{std::array{plate}}.targets().empty());
    plate.supported = true;
    plate.minimumPlayers = 2;
    CHECK(LegendsSwitches{std::array{plate}, 1}.targets().empty());
    CHECK(LegendsSwitches{std::array{plate}, 2}.targets().size() == 1);
}

TEST_CASE("Native switch motion is bounded and independent of presentation sampling",
          "[legends][switches]") {
    const std::array definitions{lift()};
    LegendsSwitches slow{definitions};
    LegendsSwitches fast{definitions};
    REQUIRE(slow.activate(0));
    REQUIRE(fast.activate(0));
    for (s32 i = 0; i < 30; ++i) {
        slow.capturePresentation();
        slow.update(1.0f / 30);
    }
    for (s32 i = 0; i < 144; ++i) {
        fast.capturePresentation();
        fast.update(1.0f / 144);
        (void)fast.targets()[0].sample(.37f);
    }
    CHECK(slow.targets()[0].offset == Approx(-6).margin(.0001));
    CHECK(fast.targets()[0].offset == Approx(-6).margin(.0001));
    const f32 held = slow.targets()[0].offset;
    for (const f32 invalid : {-1.0f, 0.0f, 11.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        slow.update(invalid);
        CHECK(slow.targets()[0].offset == held);
    }
    CHECK(slow.targets()[0].sample(5) == held);
    CHECK(slow.targets()[0].sample(std::numeric_limits<f32>::quiet_NaN()) == held);
}

TEST_CASE("Native initially active plates wait for a real simulation tick", "[legends][switches]") {
    auto plate = lift();
    plate.placementFlags = 2;
    LegendsSwitches motion{std::array{plate}};
    REQUIRE(motion.switches().size() == 1);
    CHECK_FALSE(motion.switches()[0].activated);
    motion.update(0);
    CHECK_FALSE(motion.switches()[0].activated);
    motion.capturePresentation();
    motion.update(.25f);
    CHECK(motion.switches()[0].activated);
    CHECK(motion.targets()[0].offset == -9);
    CHECK(motion.targets()[0].previous == -10);
}

TEST_CASE("Repeatable endpoint switches retain motion but release their pressed artwork",
          "[legends][switches]") {
    auto lower = lift(0);
    lower.flags = 0x2902;
    auto upper = lower;
    upper.nativeIndex = 1;
    upper.flags = 0x2a02;
    LegendsSwitches motion{std::array{lower, upper}};
    motion.contact(1, 1);
    REQUIRE(motion.switches()[1].activated);
    motion.update(1);
    CHECK(motion.targets()[0].offset == -6);
    motion.contact(1, 0);
    CHECK_FALSE(motion.switches()[1].activated);
    motion.update(2);
    CHECK(motion.targets()[0].offset == 0);
    motion.contact(0, 2);
    motion.update(1);
    CHECK(motion.targets()[0].offset == -4);
    motion.contact(0, 0);
    motion.contact(1, 4);
    motion.update(.5f);
    CHECK(motion.targets()[0].offset == -2);
    CHECK(motion.switches()[1].activated);
    motion.contact(1, 0);
    CHECK_FALSE(motion.switches()[1].activated);
}

TEST_CASE("Toggle elevators wait after arriving and do not reverse during travel",
          "[legends][switches]") {
    auto plate = lift();
    plate.flags = 0x2202;
    plate.firstEndpoint = 0;
    plate.secondEndpoint = 40;
    LegendsSwitches motion{std::array{plate}};
    motion.contact(0, 1);
    motion.update(.5f);
    CHECK(motion.targets()[0].offset == 2);
    motion.contact(0, 0);
    motion.contact(0, 2);
    motion.update(.5f);
    REQUIRE(motion.targets()[0].offset == 4);
    motion.contact(0, 2);
    motion.update(3);
    motion.contact(0, 2);
    CHECK_FALSE(motion.targets()[0].moving);
    motion.update(.01f);
    motion.contact(0, 2);
    REQUIRE(motion.targets()[0].moving);
    motion.update(.5f);
    CHECK(motion.targets()[0].offset == 2);
    motion.update(.5f);
    REQUIRE(motion.targets()[0].offset == 0);
    motion.contact(0, 0);
    motion.contact(0, 4);
    REQUIRE(motion.targets()[0].moving);
    motion.update(.25f);
    CHECK(motion.targets()[0].offset == 1);
}

TEST_CASE("Toggle elevators preserve the native zero-initialized destination",
          "[legends][switches]") {
    auto plate = lift();
    plate.flags = 0x2202;
    LegendsSwitches motion{std::array{plate}};
    motion.contact(0, 1);
    CHECK(motion.targets()[0].goal == -10);
    CHECK_FALSE(motion.targets()[0].moving);
    motion.update(3.01f);
    motion.contact(0, 1);
    CHECK(motion.targets()[0].goal == 0);
    CHECK(motion.targets()[0].moving);
}
