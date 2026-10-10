#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/netplay/ClientClock.h"
#include "game/netplay/OnlinePair.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void checkpoint(test::OnlinePair& pair, u64 tick, u64 origin = 0) {
    CombatSnapshot state;
    state.motion.epoch = pair.guest.context().epoch;
    state.motion.tick = tick;
    state.motion.cameraContinuity = 1;
    const f32 position = static_cast<f32>(tick - origin);
    state.motion.camera.position = {position, 0, 0};
    for (usize seat = 0; seat < 2; ++seat) {
        state.motion.players[seat] =
            SeatMotion{pair.guest.context().grants[seat], 1, {position, 0, 0}, 0};
        state.players[seat] =
            PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, 0, 1}};
    }
    const auto packets = CombatReplica::packets(state);
    REQUIRE(packets);
    for (const auto& packet : *packets) {
        REQUIRE(pair.guest.receive(99, packet) == MatchSession::Admission::Accepted);
    }
}

TEST_CASE("client render clock smooths checkpoints independently of local frame rate",
          "[netplay][client-clock]") {
    for (const s32 fps : {30, 60, 144}) {
        CAPTURE(fps);
        test::OnlinePair pair;
        pair.prepare();
        pair.ready();
        ClientClock clock;
        REQUIRE(clock.begin(pair.guest));
        CHECK_FALSE(clock.begin(pair.host));
        CHECK_FALSE(clock.begin(pair.guest));
        CHECK_FALSE(clock.sample(pair.guest, 1.0 / fps));
        u64 received = 0;
        f32 last = 0;
        usize fractional = 0;
        for (s32 frame = 0; frame < fps * 10; ++frame) {
            const auto hostTick = static_cast<u64>(frame) * 60 / static_cast<u64>(fps);
            const auto newest = hostTick - hostTick % 3;
            if (frame == 0 || newest > received) {
                checkpoint(pair, newest);
                received = newest;
            }
            const auto state = clock.sample(pair.guest, 1.0 / fps);
            REQUIRE(state);
            const auto x = state->motion.players[0]->position.x;
            CHECK(x >= last);
            CHECK(x <= static_cast<f32>(received));
            CHECK(x == state->motion.camera.position.x);
            CHECK(x == state->motion.players[1]->position.x);
            CHECK(static_cast<f32>(received) - x <= 10);
            fractional += std::abs(x / 3 - std::floor(x / 3)) > 0.01f ? 1U : 0U;
            last = x;
        }
        CHECK(fractional > static_cast<usize>(fps));
        CHECK(pair.host.tick() == 0); // Presentation never steps the host or client AI.
        CHECK(last > 585);
    }
}

TEST_CASE("client clock holds missing state and pause then explicitly rebinds on resume",
          "[netplay][client-clock]") {
    test::OnlinePair pair;
    pair.prepare();
    ClientClock clock;
    REQUIRE(clock.begin(pair.guest));
    CHECK_FALSE(clock.sample(pair.guest, 0));
    pair.ready();
    checkpoint(pair, 30);
    checkpoint(pair, 33);
    REQUIRE(clock.sample(pair.guest, 0));
    REQUIRE(pair.host.requestPause());
    pair.pump();
    const auto before = clock.tick();
    const auto fraction = clock.fraction();
    for (s32 frame = 0; frame < 300; ++frame) {
        REQUIRE(clock.sample(pair.guest, 1.0 / 60));
        CHECK(clock.tick() == before);
        CHECK(clock.fraction() == fraction);
    }
    pair.prepare(MatchTransition::Resume);
    CHECK_FALSE(clock.sample(pair.guest, 0));
    REQUIRE(clock.begin(pair.guest));
    pair.ready();
    CHECK_FALSE(clock.sample(pair.guest, 0));
    checkpoint(pair, 0);
    checkpoint(pair, 3);
    for (s32 frame = 0; frame < 600; ++frame) {
        const auto held = clock.sample(pair.guest, 1.0 / 60);
        REQUIRE(held);
        CHECK(held->motion.players[0]->position.x <= 3);
    }
    CHECK(clock.tick() == 3);
    CHECK(clock.fraction() == 0);
    checkpoint(pair, 600);
    REQUIRE(clock.sample(pair.guest, 5));
    CHECK(clock.tick() == 600 - ClientClock::kDelay);
    pair.guest.leave();
    CHECK_FALSE(clock.sample(pair.guest, 1));
}

TEST_CASE("client clock rejects invalid elapsed time and retains integer tick precision",
          "[netplay][client-clock]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    ClientClock clock;
    REQUIRE(clock.begin(pair.guest));
    constexpr u64 kOrigin = (u64{1} << 54U);
    checkpoint(pair, kOrigin, kOrigin);
    checkpoint(pair, kOrigin + 3, kOrigin);
    checkpoint(pair, kOrigin + 6, kOrigin);
    const auto start = clock.sample(pair.guest, 0);
    REQUIRE(start);
    CHECK(clock.tick() == kOrigin);
    CHECK(start->motion.players[0]->position.x == 0);
    for (const auto bad :
         {-1.0, std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()}) {
        CHECK_FALSE(clock.sample(pair.guest, bad));
        CHECK(clock.tick() == kOrigin);
        CHECK(clock.fraction() == 0);
    }
    const auto middle = clock.sample(pair.guest, 0.5 / 60);
    REQUIRE(middle);
    CHECK(clock.tick() == kOrigin);
    CHECK(clock.fraction() == Approx(0.5));
    CHECK(middle->motion.players[0]->position.x == Approx(0.5));
}
} // namespace
