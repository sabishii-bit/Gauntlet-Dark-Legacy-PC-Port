#include <array>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/platform/Input.h"

#include "game/players/PlayerAnimator.h"
#include "game/rollback/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("every network action maps independently to the existing gameplay input",
          "[netplay][controls]") {
    InputCommand command;
    command.epoch = 1;
    command.grant = 1;
    command.direction = {0.6f, 0.8f};
    command.magnitude = 0.75f;
    command.aimDirection = Vec3{0.6f, 0, 0.8f};
    for (u32 bit = 1; bit <= InputCommand::kPressMask; bit <<= 1U) {
        if ((bit & InputCommand::kPressMask) == 0) {
            continue; // retired wire fields must not become a different gameplay action
        }
        command.pressedButtons = bit;
        const auto play = SessionInputs::playInput(command);
        const auto result = SessionInputs::command(play, 1, 0, 1, 0);
        CHECK(result.pressedButtons == bit);
        CHECK(result.heldButtons == 0);
        CHECK(result.direction == command.direction);
        CHECK(result.magnitude == command.magnitude);
        CHECK(result.aimDirection == command.aimDirection);
    }
    command.pressedButtons = 0;
    for (u32 bit = 1; bit <= InputCommand::kHeldMask; bit <<= 1U) {
        command.heldButtons = bit;
        const auto result = SessionInputs::command(SessionInputs::playInput(command), 1, 0, 1, 0);
        CHECK(result.heldButtons == bit);
        CHECK(result.pressedButtons == 0);
    }
}

TEST_CASE("offline input retains local pointer text and device capture without quantization",
          "[netplay][controls][multiplayer]") {
    const Input devices;
    SessionInputs session;
    SessionInputs::Frame local;
    for (auto& input : local) {
        input.move = {{0.6f, 0.8f}, 0.1234567f};
        input.attack = true;
        input.attackPressed = true;
        input.selector.up = true;
        input.menu.devices = &devices;
        input.menu.typed = "TEST";
        input.menu.pointer = Vec2{0.125f, 0.75f};
        input.menu.pointerScroll = -1;
        input.menu.pointerNormalized = true;
        input.menu.pointerPressed = true;
        input.menu.pointerHeld = true;
        input.menu.pointerBack = true;
        input.menu.erase = true;
    }
    for (const auto& result : session.advance(local)) {
        CHECK(result.move.direction == local[0].move.direction);
        CHECK(result.move.magnitude == local[0].move.magnitude);
        CHECK(result.attack);
        CHECK(result.attackPressed);
        CHECK(result.selector.up);
        CHECK(result.menu.devices == &devices);
        CHECK(result.menu.typed == "TEST");
        CHECK(result.menu.pointer == local[0].menu.pointer);
        CHECK(result.menu.pointerScroll == -1);
        CHECK(result.menu.pointerNormalized);
        CHECK(result.menu.pointerPressed);
        CHECK(result.menu.pointerHeld);
        CHECK(result.menu.pointerBack);
        CHECK(result.menu.erase);
        CHECK_FALSE(result.menu.buttonPressed);
    }
    CHECK(session.timeline().tick() == 1);
    for (const auto& result : session.advance({})) {
        CHECK_FALSE(result.attack);
        CHECK_FALSE(result.attackPressed);
        CHECK_FALSE(result.selector.up);
    }
}

TEST_CASE("remote seats ignore this machine's devices and cannot retain UI pointers",
          "[netplay][controls][multiplayer]") {
    SessionInputs session;
    auto& timeline = session.timeline();
    REQUIRE(timeline.assign(2, 2));
    REQUIRE(timeline.assign(3, 0));
    SessionInputs::Frame local;
    for (auto& input : local) {
        input.attack = true;
        input.move = {{1, 0}, 1};
    }
    PlayInput remote;
    const Input devices;
    remote.menu.devices = &devices;
    remote.menu.typed = "DO NOT SEND";
    remote.menu.pointer = Vec2{1};
    remote.menu.pointerPressed = true;
    remote.movieSkipPressed = true;
    remote.move = {{-1, 0}, 1};
    remote.throwPotion = true;
    const auto command =
        SessionInputs::command(remote, timeline.epoch(), timeline.tick(), timeline.grant(2), 2);
    REQUIRE(timeline.submit(2, command) == InputTimeline::Admission::Accepted);
    const auto result = session.advance(local);
    CHECK(result[0].attack);
    CHECK(result[1].attack);
    CHECK_FALSE(result[2].attack);
    CHECK(result[2].move.direction == Vec2{-1, 0});
    CHECK(result[2].throwPotion);
    CHECK(result[2].menu.buttonPressed);
    CHECK(result[2].menu.devices == nullptr);
    CHECK_FALSE(result[2].menu.pointer);
    CHECK(result[2].menu.typed.empty());
    CHECK_FALSE(result[2].movieSkipPressed);
    CHECK_FALSE(result[3].attack);
    CHECK(result[3].move.magnitude == 0);
    CHECK(session.advance(local)[2].throwPotion);
    remote.throwPotion = false;
    REQUIRE(timeline.submit(2, SessionInputs::command(remote, timeline.epoch(), timeline.tick(),
                                                      timeline.grant(2), 2)) ==
            InputTimeline::Admission::Accepted);
    CHECK_FALSE(session.advance(local)[2].throwPotion);
}

TEST_CASE("concealed potion wind-up matches native charged release until real release or expiry",
          "[netplay][controls][potion-input]") {
    const bool expire = GENERATE(false, true);
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo root;
    root.name = "L1ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    for (const auto& [name, frames] :
         {std::pair{"READY", 60}, std::pair{"THROWPOTIONS", 11}, std::pair{"THROWPOTIONR", 9},
          std::pair{"MAGICS", 11}, std::pair{"MAGICR", 15}}) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = frames;
        sequence.frameRate = 30;
        tree.sequences.push_back(sequence);
    }
    PlayerAnimator native;
    PlayerAnimator transported;
    REQUIRE(native.bind(tree, Entrance::Skip));
    REQUIRE(transported.bind(tree, Entrance::Skip));
    InputTimeline timeline;
    REQUIRE(timeline.assign(2, 2));
    u32 nativeReleases = 0;
    u32 transportedReleases = 0;
    s32 releaseStrength = -1;
    for (u64 tick = 0; tick < 75; ++tick) {
        CAPTURE(tick, expire);
        PlayInput input;
        input.throwPotion =
            expire ? tick <= InputTimeline::kHoldTicks : tick < 8 || (tick >= 9 && tick < 12);
        // Two missing held samples must not permanently latch a false release.
        // In the full stall case, hold expires at the original sample's deadline.
        const bool deliver = expire ? tick == 0 : tick != 3 && tick != 4;
        if (deliver) {
            const auto command =
                SessionInputs::command(input, timeline.epoch(), tick, timeline.grant(2), 2);
            const auto bytes = InputPacket::encode(std::array{command});
            REQUIRE(bytes);
            const auto decoded = InputPacket::decode(*bytes);
            REQUIRE(decoded);
            REQUIRE(timeline.submit(2, decoded->front()) == InputTimeline::Admission::Accepted);
        }
        const auto commands = timeline.advance();
        const auto received = SessionInputs::playInput(commands[2]);
        CHECK(received.throwPotion == input.throwPotion);
        CHECK_FALSE(SessionInputs::playInput(commands[0]).throwPotion);
        const auto deed = [](const PlayInput& value) {
            return value.throwPotion ? PlayerDeed::ThrowPotion : PlayerDeed::None;
        };
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, deed(input));
        transported.update(PlayerMotion::Stand, 1, 1.0f / 60, deed(received));
        CHECK(transported.checkpoint() == native.checkpoint());
        nativeReleases += native.potionThrown() ? 1 : 0;
        transportedReleases += transported.potionThrown() ? 1 : 0;
        if (transported.potionThrown()) {
            releaseStrength = transported.potionThrowTicks();
        }
    }
    CHECK(nativeReleases == 1);
    CHECK(transportedReleases == 1);
    CHECK(releaseStrength == (expire ? static_cast<s32>(InputTimeline::kHoldTicks) : 7));
}

TEST_CASE("potion use remains a one-shot while held throw release immediately clears concealment",
          "[netplay][controls][potion-input]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    PlayInput input;
    input.usePotion = true;
    input.throwPotion = true;
    REQUIRE(timeline.submit(2, SessionInputs::command(input, timeline.epoch(), timeline.tick(),
                                                      timeline.grant(0), 0)) ==
            InputTimeline::Admission::Accepted);
    auto received = SessionInputs::playInput(timeline.advance()[0]);
    CHECK(received.usePotion);
    CHECK(received.throwPotion);
    received = SessionInputs::playInput(timeline.advance()[0]);
    CHECK_FALSE(received.usePotion);
    CHECK(received.throwPotion);
    input = {};
    REQUIRE(timeline.submit(2, SessionInputs::command(input, timeline.epoch(), timeline.tick(),
                                                      timeline.grant(0), 0)) ==
            InputTimeline::Admission::Accepted);
    for (u64 i = 0; i <= InputTimeline::kHoldTicks + 2; ++i) {
        received = SessionInputs::playInput(timeline.advance()[0]);
        CHECK_FALSE(received.throwPotion);
        CHECK_FALSE(received.usePotion);
    }
}
} // namespace
