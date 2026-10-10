#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/InputTimeline.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = InputTimeline::Admission;

InputCommand commandFor(const InputTimeline& timeline, u8 seat = 0) {
    InputCommand command;
    command.epoch = timeline.epoch();
    command.tick = timeline.tick();
    command.grant = timeline.grant(seat);
    command.seat = seat;
    command.direction = {1, 0};
    command.magnitude = 1;
    command.heldButtons = InputCommand::kHeldMask;
    command.pressedButtons = InputCommand::kPressMask;
    command.aimPoint = Vec3{10, 1, 0};
    return command;
}

TEST_CASE("only a seat's authenticated owner and current grant can submit inputs",
          "[netplay][ownership]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 17));
    REQUIRE(timeline.assign(3, 17)); // two local controllers on one remote machine
    auto command = commandFor(timeline);
    CHECK(timeline.submit(0, command) == Admission::WrongOwner);
    CHECK(timeline.submit(18, command) == Admission::WrongOwner);
    REQUIRE(timeline.submit(17, command) == Admission::Accepted);
    REQUIRE(timeline.submit(17, commandFor(timeline, 3)) == Admission::Accepted);
    REQUIRE(timeline.assign(0, 18));
    CHECK(timeline.submit(18, command) == Admission::WrongGrant);
    CHECK(timeline.submit(17, command) == Admission::WrongOwner);
    const auto frame = timeline.advance();
    CHECK(frame[0].magnitude == 0); // discarded old owner's pending movement
    CHECK(frame[3].magnitude == 1);
    CHECK_FALSE(timeline.assign(4, 17));
    CHECK(timeline.owner(4) == 0);
    CHECK(timeline.grant(4) == 0);
    command.seat = 255;
    CHECK(timeline.submit(17, command) == Admission::Invalid);
}

TEST_CASE("duplicate reordered and late packets cannot replay or rewrite actions",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto first = commandFor(timeline);
    auto second = first;
    second.tick = 1;
    second.magnitude = 0;
    second.pressedButtons = 0;
    REQUIRE(timeline.submit(2, second) == Admission::Accepted);
    REQUIRE(timeline.submit(2, first) == Admission::Accepted);
    auto altered = first;
    altered.direction = {-1, 0};
    CHECK(timeline.submit(2, altered) == Admission::Duplicate);
    const auto consumed = timeline.advance();
    CHECK(consumed[0].direction == first.direction);
    CHECK(consumed[0].pressedButtons == InputCommand::kPressMask);
    CHECK(timeline.submit(2, first) == Admission::TooLate);
    CHECK(timeline.advance()[0].magnitude == 0);
    CHECK(timeline.submit(2, second) == Admission::TooLate);
}

TEST_CASE("lost inputs repeat held state briefly but never potions selectors or press edges",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    CHECK(timeline.advance()[0].pressedButtons == InputCommand::kPressMask);
    for (u64 tick = 1; tick <= InputTimeline::kHoldTicks; ++tick) {
        const auto frame = timeline.advance();
        CHECK(frame[0].tick == tick);
        CHECK(frame[0].magnitude == 1);
        CHECK(frame[0].heldButtons == InputCommand::kHeldMask);
        CHECK(frame[0].pressedButtons == 0);
        CHECK(frame[0].aimPoint == Vec3{10, 1, 0});
    }
    for (s32 i = 0; i < 20; ++i) {
        const auto frame = timeline.advance();
        CHECK(frame[0].magnitude == 0);
        CHECK(frame[0].heldButtons == 0);
        CHECK(frame[0].pressedButtons == 0);
        CHECK_FALSE(frame[0].aimPoint);
    }
}

TEST_CASE("disconnect and reconnect revoke held actions queued inputs and the old grant",
          "[netplay][ownership]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.assign(1, 2));
    REQUIRE(timeline.assign(2, 3));
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    timeline.advance();
    const auto queued = commandFor(timeline);
    REQUIRE(timeline.submit(2, queued) == Admission::Accepted);
    REQUIRE(timeline.submit(2, commandFor(timeline, 1)) == Admission::Accepted);
    REQUIRE(timeline.submit(3, commandFor(timeline, 2)) == Admission::Accepted);
    timeline.disconnect(2);
    timeline.disconnect(0);
    CHECK(timeline.owner(0) == 0);
    CHECK(timeline.owner(1) == 0);
    CHECK(timeline.owner(2) == 3);
    const auto frame = timeline.advance();
    CHECK(frame[0].heldButtons == 0);
    CHECK(frame[1].pressedButtons == 0);
    CHECK(frame[2].pressedButtons == InputCommand::kPressMask);
    REQUIRE(timeline.assign(0, 2));
    CHECK(timeline.submit(2, queued) == Admission::WrongGrant);
}

TEST_CASE("epochs isolate loading and pause transitions without changing seat ownership",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    const auto old = commandFor(timeline);
    REQUIRE(timeline.submit(2, old) == Admission::Accepted);
    timeline.advance();
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    timeline.beginEpoch();
    CHECK(timeline.tick() == 0);
    CHECK(timeline.owner(0) == 2);
    CHECK(timeline.submit(2, old) == Admission::WrongEpoch);
    const auto frame = timeline.advance();
    CHECK(frame[0].magnitude == 0);
    CHECK(frame[0].heldButtons == 0);
    CHECK(frame[0].pressedButtons == 0);
    CHECK(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
}

TEST_CASE("future input storage is bounded by seats and the allowed tick window",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto command = commandFor(timeline);
    for (u64 tick = 0; tick <= InputTimeline::kMaxAhead; ++tick) {
        command.tick = tick;
        REQUIRE(timeline.submit(2, command) == Admission::Accepted);
    }
    command.tick = InputTimeline::kMaxAhead + 1;
    CHECK(timeline.submit(2, command) == Admission::TooEarly);
    command.tick = std::numeric_limits<u64>::max();
    CHECK(timeline.submit(2, command) == Admission::TooEarly);
    for (u64 tick = 0; tick <= InputTimeline::kMaxAhead; ++tick) {
        CHECK(timeline.advance()[0].tick == tick);
    }
    CHECK(timeline.advance()[0].pressedButtons == 0);
}
} // namespace
