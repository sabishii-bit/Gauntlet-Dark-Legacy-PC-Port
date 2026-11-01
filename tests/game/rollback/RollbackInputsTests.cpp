#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/rollback/RollbackInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = RollbackInputs::Admission;
constexpr u64 kEpoch = 37;

struct Fixture {
    RollbackInputs inputs;
    RollbackInputs::Owners owners;
    RollbackInputs::Grants grants{};

    explicit Fixture(RollbackInputs::Owners roster = {1, 2, 3, 4}, u8 delay = 0) : owners(roster) {
        for (usize seat = 0; seat < owners.size(); ++seat) {
            grants[seat] = owners[seat] == 0 ? 0 : static_cast<u32>(10 + seat);
        }
        REQUIRE(inputs.open(1, kEpoch, owners, grants, delay));
    }

    std::vector<InputCommand> frame(u8 peer, u64 tick, bool actions = false) const {
        std::vector<InputCommand> result;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] != peer) {
                continue;
            }
            InputCommand command;
            command.epoch = kEpoch;
            command.tick = tick;
            command.grant = grants[seat];
            command.seat = seat;
            if (actions) {
                command.direction = {1, 0};
                command.magnitude = 1;
                command.aimDirection = Vec3{0, 0, 1};
                command.heldButtons = static_cast<u32>(CommandHeld::Attack) |
                                      static_cast<u32>(CommandHeld::ThrowPotion);
                command.pressedButtons = static_cast<u32>(CommandPress::Attack) |
                                         static_cast<u32>(CommandPress::UsePotion);
                command.hintTopic = 2;
                command.hintRevision = 7;
            }
            REQUIRE(command.valid());
            result.push_back(command);
        }
        return result;
    }

    void sample(bool actions = false) {
        REQUIRE(inputs.sampleReady());
        REQUIRE(inputs.sample(frame(1, inputs.sampleTick(), actions)));
    }

    void remote(u8 peer, u64 tick, bool actions = false) {
        REQUIRE(inputs.submit(peer, frame(peer, tick, actions)) == Admission::Accepted);
    }
};

bool same(const InputCommand& a, const InputCommand& b) {
    return InputPacket::encode(std::span{&a, 1}) == InputPacket::encode(std::span{&b, 1});
}

TEST_CASE("only host input admits console actions and prediction never repeats them",
          "[netplay][rollback-inputs][console-input]") {
    constexpr RollbackInputs::Owners kOwners{1, 2, 0, 0};
    constexpr RollbackInputs::Grants kGrants{10, 11, 0, 0};
    RollbackInputs guest;
    REQUIRE(guest.open(2, kEpoch, kOwners, kGrants, 0));
    const auto command = [](u8 seat, u64 tick) {
        InputCommand value;
        value.epoch = kEpoch;
        value.tick = tick;
        value.grant = 10 + seat;
        value.seat = seat;
        return value;
    };
    auto host = command(0, 1);
    host.console = ConsoleAction{1, "god"};
    host.consoleFlight = {0, 1, 0};
    REQUIRE(guest.submit(1, std::array{host}) == Admission::Accepted);
    REQUIRE(guest.sample(std::array{command(1, 1)}));
    REQUIRE(guest.advance());
    REQUIRE(guest.sample(std::array{command(1, 2)}));
    const auto predicted = guest.advance();
    REQUIRE(predicted);
    CHECK_FALSE(predicted->commands[0].console);
    CHECK(predicted->commands[0].consoleFlight == host.consoleFlight);
    host.tick = 2;
    REQUIRE(guest.submit(1, std::array{host}) == Admission::Accepted);
    CHECK(guest.mismatchTick() == 2);

    Fixture receiving({1, 2, 0, 0});
    auto forged = receiving.frame(2, 1);
    forged.front().console = ConsoleAction{1, "god"};
    CHECK(receiving.inputs.submit(2, forged) == Admission::WrongPeer);
    CHECK_FALSE(receiving.inputs.adoptActual(forged.front()));
    CHECK_FALSE(guest.sample(std::array{forged.front()}));
}
TEST_CASE("adopted journal input is actual for every seat and sampling resumes after it",
          "[netplay][rollback-inputs][journal]") {
    RollbackInputs inputs;
    REQUIRE(inputs.open(1, 9, {1, 2, 0, 0}, {11, 12, 0, 0}, 0));
    const auto command = [](u64 tick, u8 seat, f32 x) {
        InputCommand result;
        result.epoch = 9;
        result.tick = tick;
        result.grant = seat == 0 ? 11 : 12;
        result.seat = seat;
        result.direction = {x, 0};
        result.magnitude = x == 0 ? 0.0f : 1.0f;
        return result;
    };
    for (u64 tick = 1; tick <= 10; ++tick) {
        REQUIRE(inputs.adoptActual(command(tick, 0, tick > 5 ? 1.0f : 0.0f)));
        REQUIRE(inputs.adoptActual(command(tick, 1, 0.0f)));
    }
    REQUIRE(inputs.actualFrame(10));
    CHECK((*inputs.actualFrame(10))[0].magnitude == 1);
    CHECK_FALSE(inputs.actualFrame(11));
    // Contradictions, foreign grants and other epochs are refused; repeats are not.
    CHECK_FALSE(inputs.adoptActual(command(3, 0, 1.0f)));
    CHECK(inputs.adoptActual(command(3, 0, 0.0f)));
    auto wrongGrant = command(11, 0, 0.0f);
    wrongGrant.grant = 99;
    CHECK_FALSE(inputs.adoptActual(wrongGrant));
    auto wrongEpoch = command(11, 0, 0.0f);
    wrongEpoch.epoch = 8;
    CHECK_FALSE(inputs.adoptActual(wrongEpoch));
    for (u64 tick = 1; tick <= 10; ++tick) {
        REQUIRE(inputs.ready());
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(frame->predictedSeats == 0);
    }
    CHECK(inputs.confirmedTick() == 10);
    CHECK(inputs.tick() == 11);
    // The adopted tail of local input keeps sampling ahead of the next tick.
    REQUIRE(inputs.adoptActual(command(11, 0, 1.0f)));
    REQUIRE(inputs.resumeSampling());
    CHECK(inputs.sampleTick() == 12);
    CHECK(inputs.redundantInputs().size() == 8);
    CHECK_FALSE(inputs.sampleReady()); // a tick ahead of the head with no delay
    REQUIRE(inputs.adoptActual(command(11, 1, 0.0f)));
    REQUIRE(inputs.advance());
    CHECK(inputs.sampleReady());
}

TEST_CASE("a dropped peer's seats hold neutral actual input from the first still tick",
          "[netplay][rollback-inputs][drop]") {
    RollbackInputs inputs;
    REQUIRE(inputs.open(1, 9, {1, 2, 0, 0}, {11, 12, 0, 0}, 0));
    const auto command = [](u64 tick, u8 seat, f32 x) {
        InputCommand result;
        result.epoch = 9;
        result.tick = tick;
        result.grant = seat == 0 ? 11 : 12;
        result.seat = seat;
        result.direction = {x, 0};
        result.magnitude = x == 0 ? 0.0f : 1.0f;
        return result;
    };
    // Peer two sends three ticks of movement and goes quiet.
    for (u64 tick = 1; tick <= 3; ++tick) {
        REQUIRE(inputs.submit(2, std::array{command(tick, 1, 1.0f)}) ==
                RollbackInputs::Admission::Accepted);
    }
    for (u64 tick = 1; tick <= 5; ++tick) {
        REQUIRE(inputs.sample(std::array{command(tick, 0, 0.0f)}));
        REQUIRE(inputs.advance());
    }
    CHECK(inputs.confirmedTick() == 3);
    CHECK_FALSE(inputs.suspend(1, 4)); // never the local peer
    CHECK_FALSE(inputs.suspend(3, 4)); // not in the room
    CHECK_FALSE(inputs.suspend(2, 0));
    REQUIRE(inputs.suspend(2, 4));
    CHECK(inputs.suspended(2));
    CHECK(inputs.suspend(2, 4)); // the same drop again is fine
    CHECK_FALSE(inputs.suspend(2, 5));
    // Ticks 4 and 5 ran on a held prediction of movement; neutral is actual now.
    REQUIRE(inputs.mismatchTick() == 4);
    REQUIRE(inputs.rewind(4));
    for (u64 tick = 4; tick <= 5; ++tick) {
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(frame->predictedSeats == 0);
        CHECK(frame->commands[1].magnitude == 0);
        CHECK(frame->commands[1].grant == 12);
    }
    CHECK(inputs.confirmedTick() == 5);
    // Later input from the dropped peer is ignored; an earlier resend is still taken.
    CHECK(inputs.submit(2, std::array{command(6, 1, 1.0f)}) == RollbackInputs::Admission::Stale);
    CHECK(inputs.submit(2, std::array{command(2, 1, 1.0f)}) ==
          RollbackInputs::Admission::Duplicate);
    // Nothing waits on the dropped seat from here on, and the journal sees its stillness.
    REQUIRE(inputs.sample(std::array{command(6, 0, 0.0f)}));
    REQUIRE(inputs.ready());
    const auto frame = inputs.advance();
    REQUIRE(frame);
    CHECK(frame->predictedSeats == 0);
    REQUIRE(inputs.actualFrame(6));
    CHECK((*inputs.actualFrame(6))[1].magnitude == 0);
    CHECK(inputs.confirmedTick() == 6);
    CHECK_FALSE(inputs.timeSyncIdle());
}

TEST_CASE("a drop holds still the input a direct link brought early, past what has settled",
          "[netplay][rollback-inputs][drop]") {
    const u64 settled = GENERATE(u64{0}, u64{4});
    CAPTURE(settled);
    // Guest three of three, which hears peer two directly, sooner than the host does.
    RollbackInputs inputs;
    REQUIRE(inputs.open(3, 9, {1, 2, 3, 0}, {11, 12, 13, 0}, 0));
    const auto command = [](u64 tick, u8 seat, f32 x) {
        InputCommand result;
        result.epoch = 9;
        result.tick = tick;
        result.grant = 11 + seat;
        result.seat = seat;
        result.direction = {x, 0};
        result.magnitude = x == 0 ? 0.0f : 1.0f;
        return result;
    };
    for (u64 tick = 1; tick <= 5; ++tick) {
        REQUIRE(inputs.submit(1, std::array{command(tick, 0, 0.0f)}) ==
                RollbackInputs::Admission::Accepted);
        REQUIRE(inputs.submit(2, std::array{command(tick, 1, 1.0f)}) ==
                RollbackInputs::Admission::Accepted);
        REQUIRE(inputs.sample(std::array{command(tick, 2, 0.0f)}));
        REQUIRE(inputs.advance());
    }
    REQUIRE(inputs.confirmedTick() == 5);
    // The host had peer two's input only through tick three, so the shared drop starts at four.
    REQUIRE(inputs.suspend(2, 4, settled));
    const u64 revised = std::max<u64>(4, settled + 1);
    REQUIRE(inputs.mismatchTick() == revised);
    for (u64 tick = 4; tick <= 5; ++tick) {
        CAPTURE(tick);
        REQUIRE(inputs.actualFrame(tick));
        // A settled tick keeps what it ran on; resync repairs it, nothing can undo it here.
        CHECK((*inputs.actualFrame(tick))[1].magnitude == (tick < revised ? 1.0f : 0.0f));
    }
    REQUIRE(inputs.rewind(revised));
    for (u64 tick = revised; tick <= 5; ++tick) {
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(frame->predictedSeats == 0);
        CHECK(frame->commands[1].magnitude == 0);
    }
    CHECK(inputs.confirmedTick() == 5);
}

TEST_CASE("a dropped seat takes none of the recorded input from its drop on",
          "[netplay][rollback-inputs][drop][journal]") {
    // A rebuild adopts the host's journal and the input it held itself; a dropped peer's seat
    // stays still from its drop whatever either recorded there.
    RollbackInputs inputs;
    REQUIRE(inputs.open(3, 9, {1, 2, 3, 0}, {11, 12, 13, 0}, 0));
    const auto command = [](u64 tick, u8 seat, f32 x) {
        InputCommand result;
        result.epoch = 9;
        result.tick = tick;
        result.grant = 11 + seat;
        result.seat = seat;
        result.direction = {x, 0};
        result.magnitude = x == 0 ? 0.0f : 1.0f;
        return result;
    };
    REQUIRE(inputs.suspend(2, 4));
    for (const u64 tick : {u64{3}, u64{5}}) {
        REQUIRE(inputs.adoptActual(command(tick, 0, 0.0f)));
        REQUIRE(inputs.adoptActual(command(tick, 1, 1.0f)));
        REQUIRE(inputs.adoptActual(command(tick, 2, 0.0f)));
    }
    const auto before = inputs.actualFrame(3);
    const auto after = inputs.actualFrame(5);
    REQUIRE(before);
    REQUIRE(after);
    CHECK((*before)[1].magnitude == 1.0f);
    CHECK((*after)[1].magnitude == 0.0f);
    CHECK((*after)[1].grant == 12);
}

TEST_CASE("a dropped peer's guessed stillness still runs again from its first still tick",
          "[netplay][rollback-inputs][drop]") {
    const auto command = [](u64 tick, u8 seat) {
        InputCommand result;
        result.epoch = 9;
        result.tick = tick;
        result.grant = seat == 0 ? 11 : 12;
        result.seat = seat;
        return result;
    };
    RollbackInputs inputs;
    REQUIRE(inputs.open(1, 9, {1, 2, 0, 0}, {11, 12, 0, 0}, 0));
    // Peer two stands still for three ticks and goes quiet; its stillness is guessed right.
    for (u64 tick = 1; tick <= 3; ++tick) {
        REQUIRE(inputs.submit(2, std::array{command(tick, 1)}) == Admission::Accepted);
    }
    for (u64 tick = 1; tick <= 6; ++tick) {
        REQUIRE(inputs.sample(std::array{command(tick, 0)}));
        REQUIRE(inputs.advance());
    }
    REQUIRE(inputs.confirmedTick() == 3);
    REQUIRE_FALSE(inputs.mismatchTick());
    REQUIRE(inputs.suspend(2, 4));
    CHECK_FALSE(inputs.suspendedSeat(1, 3));
    CHECK(inputs.suspendedSeat(1, 4));
    CHECK_FALSE(inputs.suspendedSeat(0, 6));
    CHECK_FALSE(inputs.suspendedSeat(InputCommand::kSeats, 6));
    // Its seat leaves play at tick four, so the ticks run from there run again.
    REQUIRE(inputs.mismatchTick() == 4);
    REQUIRE(inputs.rewind(4));
    for (u64 tick = 4; tick <= 6; ++tick) {
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(frame->predictedSeats == 0);
    }
    CHECK(inputs.confirmedTick() == 6);

    // Ticks already settled are never asked to run again.
    RollbackInputs settled;
    REQUIRE(settled.open(1, 9, {1, 2, 0, 0}, {11, 12, 0, 0}, 0));
    for (u64 tick = 1; tick <= 6; ++tick) {
        REQUIRE(settled.submit(2, std::array{command(tick, 1)}) == Admission::Accepted);
        REQUIRE(settled.sample(std::array{command(tick, 0)}));
        REQUIRE(settled.advance());
    }
    REQUIRE(settled.confirmedTick() == 6);
    REQUIRE(settled.suspend(2, 4, 6));
    CHECK_FALSE(settled.mismatchTick());
}

/** Peer one hosts; peers two and three are guests, and three also hears two directly. */
constexpr RollbackInputs::Owners kGuestsLinked{1, 2, 3, 0};
constexpr RollbackInputs::Grants kGuestsLinkedGrants{11, 12, 13, 0};
InputCommand linkedCommand(u64 tick, u8 seat, f32 x) {
    InputCommand result;
    result.epoch = 9;
    result.tick = tick;
    result.grant = kGuestsLinkedGrants[seat];
    result.seat = seat;
    result.direction = {x, 0};
    result.magnitude = x == 0 ? 0.0f : 1.0f;
    return result;
}

TEST_CASE("every survivor holds a dropped guest still from the host's tick, whatever it held",
          "[netplay][rollback-inputs][drop][direct-link]") {
    const bool executed = GENERATE(false, true);
    CAPTURE(executed);
    RollbackInputs host;
    RollbackInputs guest;
    REQUIRE(host.open(1, 9, kGuestsLinked, kGuestsLinkedGrants, 0));
    REQUIRE(guest.open(3, 9, kGuestsLinked, kGuestsLinkedGrants, 0));
    for (u64 tick = 1; tick <= 3; ++tick) {
        REQUIRE(host.sample(std::array{linkedCommand(tick, 0, 0.0f)}));
        REQUIRE(guest.sample(std::array{linkedCommand(tick, 2, 0.0f)}));
        REQUIRE(host.submit(2, std::array{linkedCommand(tick, 1, 0.0f)}) == Admission::Accepted);
        REQUIRE(host.submit(3, std::array{linkedCommand(tick, 2, 0.0f)}) == Admission::Accepted);
        REQUIRE(guest.submit(1, std::array{linkedCommand(tick, 0, 0.0f)}) == Admission::Accepted);
        REQUIRE(guest.submit(2, std::array{linkedCommand(tick, 1, 0.0f)}) == Admission::Accepted);
        REQUIRE(host.advance());
        REQUIRE(guest.advance());
    }
    // Peer two's fourth tick of movement reaches peer three directly; the host never gets it.
    REQUIRE(guest.submit(2, std::array{linkedCommand(4, 1, 1.0f)}) == Admission::Accepted);
    REQUIRE(host.sample(std::array{linkedCommand(4, 0, 0.0f)}));
    REQUIRE(guest.sample(std::array{linkedCommand(4, 2, 0.0f)}));
    REQUIRE(host.submit(3, std::array{linkedCommand(4, 2, 0.0f)}) == Admission::Accepted);
    if (executed) {
        REQUIRE(guest.advance()); // on that movement, before the host's own input is in
        CHECK(guest.confirmedTick() == 3);
    }
    // The host drops peer two from the tick after the last input it received.
    REQUIRE(host.suspend(2, 4));
    REQUIRE(guest.suspend(2, 4));
    REQUIRE(guest.submit(1, std::array{linkedCommand(4, 0, 0.0f)}) == Admission::Accepted);
    if (executed) {
        REQUIRE(guest.mismatchTick() == 4);
        REQUIRE(guest.rewind(4));
    }
    REQUIRE(host.advance());
    const auto frame = guest.advance();
    REQUIRE(frame);
    CHECK(frame->predictedSeats == 0);
    CHECK(frame->commands[1].magnitude == 0.0f);
    for (u64 tick = 1; tick <= 4; ++tick) {
        CAPTURE(tick);
        const auto hostFrame = host.actualFrame(tick);
        const auto guestFrame = guest.actualFrame(tick);
        REQUIRE(hostFrame);
        REQUIRE(guestFrame);
        for (usize seat = 0; seat < 3; ++seat) {
            const auto expected = InputPacket::encode(std::span{&(*hostFrame)[seat], 1});
            REQUIRE(expected);
            CHECK(InputPacket::encode(std::span{&(*guestFrame)[seat], 1}) == expected);
        }
    }
    CHECK(host.confirmedTick() == 4);
    CHECK(guest.confirmedTick() == 4);
}

TEST_CASE("a guest confirms another guest's input only as far as the host holds it",
          "[netplay][rollback-inputs][drop][direct-link]") {
    RollbackInputs host;
    REQUIRE(host.open(1, 9, kGuestsLinked, kGuestsLinkedGrants, 0));
    CHECK_FALSE(host.observeHostReceipt(2, 9)); // the host's receipt is its own
    RollbackInputs guest;
    REQUIRE(guest.open(3, 9, kGuestsLinked, kGuestsLinkedGrants, 0));
    CHECK_FALSE(guest.observeHostReceipt(1, 9)); // the host's own input is all the host's
    CHECK_FALSE(guest.observeHostReceipt(3, 9)); // as the guest's own is its own
    CHECK_FALSE(guest.observeHostReceipt(4, 9)); // and nobody sits at the fourth
    for (u64 tick = 1; tick <= 4; ++tick) {
        REQUIRE(guest.sample(std::array{linkedCommand(tick, 2, 0.0f)}));
        REQUIRE(guest.submit(1, std::array{linkedCommand(tick, 0, 0.0f)}) == Admission::Accepted);
        // Peer two's fourth tick comes over the direct link before the host has it.
        REQUIRE(guest.submit(2, std::array{linkedCommand(tick, 1, tick == 4 ? 1.0f : 0.0f)}) ==
                Admission::Accepted);
        REQUIRE(guest.observeHostReceipt(2, std::min<u64>(tick, 3)));
        REQUIRE(guest.advance());
    }
    CHECK(guest.confirmedTick() == 3);
    REQUIRE(guest.observeHostReceipt(2, 2)); // an older report lowers nothing
    CHECK(guest.confirmedTick() == 3);
    SECTION("the host receives it") {
        REQUIRE(guest.observeHostReceipt(2, 4));
        CHECK(guest.confirmedTick() == 4);
    }
    SECTION("the host drops peer two first") {
        REQUIRE(guest.suspend(2, 4));
        REQUIRE(guest.mismatchTick() == 4);
        REQUIRE(guest.rewind(4));
        const auto frame = guest.advance();
        REQUIRE(frame);
        CHECK(frame->commands[1].magnitude == 0.0f);
        // The drop says the host holds all before it; nothing waits on peer two any more.
        CHECK(guest.confirmedTick() == 4);
    }
}

TEST_CASE("the startup delay follows the measured round trip", "[netplay][rollback-inputs]") {
    CHECK(RollbackInputs::leadForPing(-1) == 1);
    CHECK(RollbackInputs::leadForPing(0) == 0);
    CHECK(RollbackInputs::leadForPing(19) == 0);
    CHECK(RollbackInputs::leadForPing(20) == 1);
    CHECK(RollbackInputs::leadForPing(39) == 1);
    CHECK(RollbackInputs::leadForPing(40) == 2);
    CHECK(RollbackInputs::leadForPing(69) == 2);
    CHECK(RollbackInputs::leadForPing(70) == 3); // half an eighty-millisecond trip is 2.4 ticks
    CHECK(RollbackInputs::leadForPing(104) == 3);
    CHECK(RollbackInputs::leadForPing(105) == 4);
    CHECK(RollbackInputs::leadForPing(400) == RollbackInputs::kMaxDelay);
    CHECK(RollbackInputs::kMaxDelay < RollbackInputs::kPredictionTicks);
}

} // namespace

TEST_CASE("rollback local buffering is negotiated and never predicts a local seat",
          "[rollback-inputs]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    Fixture fixture({1, 2, 0, 0}, delay);
    auto& inputs = fixture.inputs;
    CHECK(inputs.sampleTick() == 1 + delay);
    CHECK(inputs.confirmedTick() == 0); // Known inputs alone are not executed state.
    for (u64 tick = 1; tick <= 20; ++tick) {
        CAPTURE(delay, tick);
        fixture.remote(2, inputs.sampleTick(), true);
        fixture.sample(true);
        CHECK_FALSE(inputs.sampleReady());
        const auto used = inputs.advance();
        REQUIRE(used);
        CHECK(used->predictedSeats == 0);
        CHECK(used->commands[0].pressed(CommandPress::UsePotion) == (tick > delay));
        CHECK(inputs.confirmedTick() == tick);
    }
    for (u8 buffered = 0; buffered < delay; ++buffered) {
        REQUIRE(inputs.advance());
    }
    CHECK_FALSE(inputs.ready());
    CHECK_FALSE(inputs.advance());
}

TEST_CASE("missing remote inputs retain held aim and throw charging but never replay an edge",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    fixture.sample();
    fixture.remote(2, 1, true);
    REQUIRE(fixture.inputs.advance());
    CHECK(fixture.inputs.confirmedTick() == 1);
    for (u64 tick = 2; tick <= 9; ++tick) {
        fixture.sample();
        const auto predicted = fixture.inputs.advance();
        REQUIRE(predicted);
        CHECK(predicted->predictedSeats == 2);
        const auto& command = predicted->commands[1];
        CHECK(command.tick == tick);
        CHECK(command.magnitude == 1);
        CHECK(command.direction == Vec2{1, 0});
        CHECK(command.aimDirection == std::optional{Vec3{0, 0, 1}});
        CHECK(command.held(CommandHeld::Attack));
        CHECK(command.held(CommandHeld::ThrowPotion));
        CHECK(command.pressedButtons == 0);
        CHECK(command.hintTopic == 0);
        CHECK(command.hintRevision == 0);
    }
    CHECK(fixture.inputs.tick() == 10);
    CHECK_FALSE(fixture.inputs.ready());
    CHECK_FALSE(fixture.inputs.advance());
}

TEST_CASE("confirmation requires every seat including multiple seats owned by one peer",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 2, 3});
    fixture.sample();
    auto partial = fixture.frame(2, 1);
    partial.pop_back();
    CHECK(fixture.inputs.submit(2, partial) == Admission::Invalid);
    fixture.remote(2, 2);
    fixture.remote(3, 2);
    const auto used = fixture.inputs.advance();
    REQUIRE(used);
    CHECK(used->predictedSeats == 14);
    fixture.remote(2, 1);
    CHECK(fixture.inputs.confirmedTick() == 0);
    CHECK_FALSE(fixture.inputs.mismatchTick()); // Actual neutral agrees with guessed neutral.
    fixture.remote(3, 1);
    CHECK(fixture.inputs.confirmedTick() == 1);
    fixture.sample();
    REQUIRE(fixture.inputs.advance());
    CHECK(fixture.inputs.confirmedTick() == 2);
}

TEST_CASE(
    "three and four peer corrections replay one through eight frames with immutable local inputs",
    "[rollback-inputs]") {
    const u8 peers = GENERATE(u8{3}, u8{4});
    const u64 distance = GENERATE(u64{1}, u64{2}, u64{3}, u64{4}, u64{5}, u64{6}, u64{7}, u64{8});
    CAPTURE(peers, distance);
    Fixture fixture({1, 2, 3, static_cast<u8>(peers == 4 ? 4 : 0)});
    for (u64 tick = 1; tick <= distance; ++tick) {
        fixture.sample(tick % 2 != 0);
        REQUIRE(fixture.inputs.advance());
    }
    CHECK(fixture.inputs.confirmedTick() == 0);
    const u64 nextSample = fixture.inputs.sampleTick();
    const auto localEvidence = std::vector<InputCommand>{fixture.inputs.redundantInputs().begin(),
                                                         fixture.inputs.redundantInputs().end()};
    // Reordered/lost packets arrive in a single eight-tick redundant window.
    // Each seat's actual inputs still exist; no late edge is discarded.
    for (u8 peer = 2; peer <= peers; ++peer) {
        std::vector<InputCommand> recovered;
        for (u64 tick = distance; tick > 0; --tick) {
            const auto commands = fixture.frame(peer, tick, tick % 2 != 0);
            recovered.insert(recovered.end(), commands.begin(), commands.end());
        }
        REQUIRE(fixture.inputs.submit(peer, recovered) == Admission::Accepted);
        CHECK(fixture.inputs.submit(peer, recovered) == Admission::Duplicate);
    }
    REQUIRE(fixture.inputs.mismatchTick() == 1);
    CHECK(fixture.inputs.confirmedTick() == 0); // Actual complete, but wrong state not corrected.
    CHECK_FALSE(fixture.inputs.ready());
    CHECK_FALSE(fixture.inputs.rewind(2));
    REQUIRE(fixture.inputs.rewind(1));
    CHECK(fixture.inputs.replaying());
    CHECK_FALSE(fixture.inputs.sampleReady());
    CHECK(fixture.inputs.sampleTick() == nextSample);
    for (u64 tick = 1; tick <= distance; ++tick) {
        const auto corrected = fixture.inputs.advance();
        REQUIRE(corrected);
        CHECK(corrected->predictedSeats == 0);
        for (u8 peer = 1; peer <= peers; ++peer) {
            for (const auto& actual : fixture.frame(peer, tick, tick % 2 != 0)) {
                CHECK(same(corrected->commands[actual.seat], actual));
            }
        }
        CHECK(fixture.inputs.confirmedTick() == tick);
    }
    CHECK_FALSE(fixture.inputs.replaying());
    CHECK_FALSE(fixture.inputs.mismatchTick());
    CHECK(fixture.inputs.sampleTick() == nextSample);
    REQUIRE(fixture.inputs.redundantInputs().size() == localEvidence.size());
    for (usize i = 0; i < localEvidence.size(); ++i) {
        CHECK(same(fixture.inputs.redundantInputs()[i], localEvidence[i]));
    }
}

TEST_CASE(
    "multiple late corrections select the earliest disagreement and preserve later actual evidence",
    "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.sample();
        REQUIRE(fixture.inputs.advance());
    }
    fixture.remote(2, 6, true);
    CHECK(fixture.inputs.mismatchTick() == 6);
    fixture.remote(2, 3, true);
    CHECK(fixture.inputs.mismatchTick() == 3);
    REQUIRE(fixture.inputs.rewind(3));
    for (u64 tick = 3; tick <= 8; ++tick) {
        const auto used = fixture.inputs.advance();
        REQUIRE(used);
        CHECK(used->commands[1].held(CommandHeld::Attack));
        CHECK(used->commands[1].pressed(CommandPress::UsePotion) == (tick == 3 || tick == 6));
    }
    CHECK_FALSE(fixture.inputs.ready()); // Ticks 1/2 are still not confirmed.
}

TEST_CASE(
    "rollback admission checks complete batches atomically and detects conflicting actual input",
    "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    fixture.remote(2, 1);
    auto conflict = fixture.frame(2, 1, true);
    const auto later = fixture.frame(2, 2);
    conflict.insert(conflict.begin(), later.begin(), later.end());
    CHECK(fixture.inputs.submit(2, conflict) == Admission::Conflict);
    fixture.remote(2, 2); // The rejected batch did not admit its earlier new command.
    CHECK(fixture.inputs.submit(2, fixture.frame(2, 1)) == Admission::Duplicate);
    auto invalid = fixture.frame(2, 3);
    invalid[0].epoch++;
    CHECK(fixture.inputs.submit(2, invalid) == Admission::WrongEpoch);
    invalid = fixture.frame(2, 3);
    invalid[0].grant++;
    CHECK(fixture.inputs.submit(2, invalid) == Admission::WrongGrant);
    CHECK(fixture.inputs.submit(3, fixture.frame(2, 3)) == Admission::WrongPeer);
    CHECK(fixture.inputs.submit(2, fixture.frame(1, 3)) == Admission::WrongPeer);
    CHECK(fixture.inputs.submit(1, fixture.frame(1, 3)) == Admission::WrongPeer);
    invalid = fixture.frame(2, 3);
    invalid[0].direction.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK(fixture.inputs.submit(2, invalid) == Admission::Invalid);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, 0)) == Admission::Invalid);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, std::numeric_limits<u64>::max())) ==
          Admission::TooEarly);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, fixture.inputs.tick() + 121)) ==
          Admission::TooEarly);
    invalid = fixture.frame(2, 3);
    invalid.push_back(invalid.front());
    CHECK(fixture.inputs.submit(2, invalid) == Admission::Invalid);
    auto negativeZero = fixture.frame(2, 1);
    negativeZero[0].direction.x = -0.0f;
    CHECK(fixture.inputs.submit(2, negativeZero) == Admission::Conflict);
}

TEST_CASE("history wraps without losing eight redundant ticks or accepting stale revisions",
          "[rollback-inputs]") {
    Fixture fixture({1, 1, 1, 1});
    for (u64 tick = 1; tick <= 300; ++tick) {
        fixture.sample(tick % 3 == 0);
        REQUIRE(fixture.inputs.advance());
        CHECK(fixture.inputs.confirmedTick() == tick);
    }
    REQUIRE(fixture.inputs.redundantInputs().size() == 32);
    CHECK(fixture.inputs.redundantInputs().front().tick == 293);
    CHECK(fixture.inputs.redundantInputs().back().tick == 300);
    CHECK_FALSE(fixture.inputs.rewind(293)); // Actual all-local frames have no mismatch.

    Fixture remote({1, 2, 0, 0});
    for (u64 tick = 1; tick <= 300; ++tick) {
        remote.sample();
        remote.remote(2, tick);
        REQUIRE(remote.inputs.advance());
    }
    CHECK(remote.inputs.submit(2, remote.frame(2, 1)) == Admission::Stale);
    CHECK(remote.inputs.submit(2, remote.frame(2, 300)) == Admission::Duplicate);
    CHECK(remote.inputs.submit(2, remote.frame(2, 300, true)) == Admission::Conflict);
    CHECK(remote.inputs.confirmedTick() == 300);
}

TEST_CASE("time synchronization follows authenticated simulated progress not future input lead",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 3, 0});
    // These are received zero-latency reports from peers that remain at tick
    // one, not unknown peers awaiting their first network report.
    REQUIRE(fixture.inputs.observePeerTick(2, kEpoch, 1) == Admission::Duplicate);
    REQUIRE(fixture.inputs.observePeerTick(3, kEpoch, 1) == Admission::Duplicate);
    for (u64 tick = 1; tick <= 4; ++tick) {
        fixture.sample();
        REQUIRE(fixture.inputs.advance());
    }
    REQUIRE(fixture.inputs.timeSyncIdle());
    fixture.remote(2, 100);
    fixture.remote(3, 100);
    CHECK(fixture.inputs.timeSyncIdle());
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, 5) == Admission::Accepted);
    CHECK(fixture.inputs.timeSyncIdle()); // The other peer is still behind.
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 5) == Admission::Accepted);
    CHECK_FALSE(fixture.inputs.timeSyncIdle());
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 5) == Admission::Duplicate);
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 1) == Admission::Stale);
    CHECK(fixture.inputs.observePeerTick(3, kEpoch + 1, 6) == Admission::WrongEpoch);
    CHECK(fixture.inputs.observePeerTick(4, kEpoch, 6) == Admission::WrongPeer);
    CHECK(fixture.inputs.observePeerTick(1, kEpoch, 6) == Admission::WrongPeer);
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, 0) == Admission::Invalid);
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, std::numeric_limits<u64>::max()) ==
          Admission::TooEarly);
    fixture.remote(2, 1, true);
    REQUIRE(fixture.inputs.rewind(1));
    CHECK_FALSE(fixture.inputs.timeSyncIdle()); // Replay does not intentionally idle.
}

TEST_CASE("unknown startup progress permits only the prediction window then a measured peer paces",
          "[rollback-inputs][rollback-pacing]") {
    Fixture fixture({1, 2, 0, 0});
    auto& inputs = fixture.inputs;
    for (u64 tick = 1; tick <= RollbackInputs::kPredictionTicks; ++tick) {
        CHECK_FALSE(inputs.timeSyncIdle());
        fixture.sample();
        REQUIRE(inputs.advance());
    }
    CHECK(inputs.tick() == 9);
    CHECK(inputs.confirmedTick() == 0);
    CHECK_FALSE(inputs.timeSyncIdle());
    fixture.sample();
    CHECK_FALSE(inputs.ready()); // Unknown latency never extends speculation.
    CHECK_FALSE(inputs.advance());

    REQUIRE(inputs.observePeerTick(2, kEpoch, 2, 1) == Admission::Accepted);
    CHECK(inputs.timeSyncIdle()); // The first measured report tightens the allowance.
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.remote(2, tick);
    }
    CHECK(inputs.confirmedTick() == 8);
    CHECK(inputs.ready());
    CHECK(inputs.timeSyncIdle()); // Buffered input does not mean a slow peer simulated it.
    REQUIRE(inputs.observePeerTick(2, kEpoch, 9, 1) == Admission::Accepted);
    CHECK_FALSE(inputs.timeSyncIdle());
    REQUIRE(inputs.advance());
    CHECK(inputs.tick() == 10);
}

TEST_CASE("a stalled peer can retransmit oldest missing input beyond the latest redundant window",
          "[rollback-inputs]") {
    Fixture sender({1, 2, 0, 0}, 2);
    for (u64 tick = 1; tick <= 10; ++tick) {
        sender.sample(true);
        REQUIRE(sender.inputs.advance());
    }
    CHECK(sender.inputs.confirmedTick() == 2);
    CHECK_FALSE(sender.inputs.ready());
    REQUIRE(sender.inputs.redundantInputs().size() == 8);
    CHECK(sender.inputs.redundantInputs().front().tick == 5);

    RollbackInputs receiver;
    REQUIRE(receiver.open(2, kEpoch, sender.owners, sender.grants, 2));
    const auto recovery = sender.inputs.resendInputs(3);
    REQUIRE(recovery.size() == 8);
    CHECK(recovery.front().tick == 3);
    CHECK(recovery.back().tick == 10);
    REQUIRE(receiver.submit(1, recovery) == Admission::Accepted);
    CHECK(receiver.submit(1, recovery) == Admission::Duplicate);
    for (u64 tick = 1; tick <= 10; ++tick) {
        REQUIRE(receiver.sample(sender.frame(2, receiver.sampleTick())));
        const auto used = receiver.advance();
        REQUIRE(used);
        CHECK(used->predictedSeats == 0);
        CHECK(used->commands[0].pressed(CommandPress::UsePotion) == (tick > 2));
        CHECK(receiver.confirmedTick() == tick);
    }
    const auto following = sender.inputs.resendInputs(11);
    REQUIRE(following.size() == 2);
    CHECK(following.back().tick == 12);
    CHECK(sender.inputs.resendInputs(0).empty());
    CHECK(sender.inputs.resendInputs(13).empty());
}

TEST_CASE("equal rate peers allow in-flight progress age instead of throttling one another",
          "[rollback-inputs]") {
    // Eighty milliseconds is roughly three 30 Hz or five 60 Hz simulation ticks.
    // Both inputs and progress are delayed, without fabricating future input lead.
    const u8 transitTicks = GENERATE(u8{3}, u8{5});
    Fixture first({1, 2, 0, 0});
    RollbackInputs second;
    REQUIRE(second.open(2, kEpoch, first.owners, first.grants, 0));
    for (u64 tick = 1; tick <= 30; ++tick) {
        CAPTURE(transitTicks, tick);
        const u64 delivered = tick > transitTicks ? tick - transitTicks : 1;
        const auto firstProgress = first.inputs.observePeerTick(2, kEpoch, delivered, transitTicks);
        const auto secondProgress = second.observePeerTick(1, kEpoch, delivered, transitTicks);
        REQUIRE((firstProgress == Admission::Accepted || firstProgress == Admission::Duplicate));
        REQUIRE(secondProgress == firstProgress);
        if (tick > transitTicks) {
            first.remote(2, delivered);
            REQUIRE(second.submit(1, first.frame(1, delivered)) == Admission::Accepted);
        }
        CHECK_FALSE(first.inputs.timeSyncIdle());
        CHECK_FALSE(second.timeSyncIdle());
        first.sample();
        REQUIRE(second.sample(first.frame(2, second.sampleTick())));
        REQUIRE(first.inputs.advance());
        REQUIRE(second.advance());
    }
    // Fresh transport latency is not a license to run arbitrarily ahead.
    for (u64 extra = 0; extra < 2; ++extra) {
        first.sample();
        REQUIRE(first.inputs.advance());
    }
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, first.inputs.tick(), 9) == Admission::Invalid);
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, 1, 8) == Admission::Stale);
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, first.inputs.tick() - transitTicks,
                                       transitTicks) == Admission::Accepted);
    CHECK_FALSE(first.inputs.timeSyncIdle());
}

TEST_CASE("rollback startup validates frozen roster and cannot reopen an existing epoch owner",
          "[rollback-inputs]") {
    RollbackInputs inputs;
    CHECK_FALSE(inputs.ready());
    CHECK_FALSE(inputs.sampleReady());
    CHECK_FALSE(inputs.advance());
    CHECK_FALSE(inputs.rewind(1));
    CHECK(inputs.confirmedTick() == 0);
    const RollbackInputs::Owners owners{1, 2, 0, 0};
    const RollbackInputs::Grants grants{1, 1, 0, 0};
    CHECK_FALSE(inputs.open(0, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(3, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(1, 0, owners, grants));
    CHECK_FALSE(inputs.open(1, kEpoch, owners, grants, RollbackInputs::kMaxDelay + 1));
    CHECK_FALSE(inputs.open(1, kEpoch, owners, {1, 0, 0, 0}));
    CHECK_FALSE(inputs.open(1, kEpoch, {1, 5, 0, 0}, grants));
    REQUIRE(inputs.open(1, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(1, kEpoch + 1, owners, grants));
}

TEST_CASE("an earlier terminal truncates only active replay and preserves sampled future inputs",
          "[rollback-inputs][rollback-terminal]") {
    Fixture fixture({1, 2, 3, 0});
    auto& inputs = fixture.inputs;
    CHECK_FALSE(inputs.finishReplayAt(1));
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.sample(tick % 2 == 0);
        REQUIRE(inputs.advance());
    }
    const auto before = inputs.resendInputs(1);
    const auto sampleCursor = inputs.sampleTick();
    fixture.remote(2, 3, true);
    REQUIRE(inputs.rewind(3));
    CHECK_FALSE(inputs.finishReplayAt(3)); // A terminal needs an actually replayed frame.
    REQUIRE(inputs.advance());
    CHECK_FALSE(inputs.finishReplayAt(3));
    CHECK_FALSE(inputs.finishReplayAt(5));
    REQUIRE(inputs.finishReplayAt(4));
    CHECK_FALSE(inputs.replaying());
    CHECK_FALSE(inputs.finishReplayAt(4));
    CHECK(inputs.tick() == 4);
    CHECK(inputs.sampleTick() == sampleCursor);
    CHECK_FALSE(inputs.sampleReady()); // Already sampled input cannot be polled again.
    const auto after = inputs.resendInputs(1);
    REQUIRE(after.size() == before.size());
    for (usize i = 0; i < before.size(); ++i) {
        CHECK(same(before[i], after[i]));
    }
    fixture.remote(2, 6, true);
    CHECK_FALSE(inputs.mismatchTick()); // Discarded speculative tail is no longer executed.
    fixture.remote(3, 2, true);         // An earlier correction can remove the tentative terminal.
    REQUIRE(inputs.mismatchTick() == 2);
    REQUIRE(inputs.rewind(2));
    for (u64 tick = 2; tick <= 8; ++tick) {
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(same(frame->commands[0], fixture.frame(1, tick, tick % 2 == 0).front()));
        if (tick == 6) {
            CHECK(frame->commands[1].pressed(CommandPress::UsePotion));
        }
    }
    CHECK(inputs.sampleTick() == sampleCursor);
    CHECK(inputs.sampleReady());
}
