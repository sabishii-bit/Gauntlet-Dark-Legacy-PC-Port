#include <bit>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/netplay/SnapshotPlayback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Admission = SnapshotPlayback::Admission;

MotionSnapshot state(u64 tick = 0) {
    MotionSnapshot result;
    result.epoch = 9;
    result.tick = tick;
    result.cameraContinuity = 1;
    result.camera = {{-10, 20, 30}, 0.5f, 1, -0.1f};
    for (usize seat = 0; seat < result.players.size(); ++seat) {
        result.players[seat] =
            SeatMotion{static_cast<u32>(seat + 1), 1, {static_cast<f32>(seat * 3), 0, 2}, 0};
    }
    return result;
}

std::vector<u8> encode(const MotionSnapshot& snapshot) {
    const auto bytes = MotionPacket::encode(snapshot);
    REQUIRE(bytes);
    return *bytes;
}

void equalState(const MotionSnapshot& actual, const MotionSnapshot& expected) {
    CHECK(actual.epoch == expected.epoch);
    CHECK(actual.tick == expected.tick);
    CHECK(actual.cameraContinuity == expected.cameraContinuity);
    CHECK(actual.camera.position == expected.camera.position);
    CHECK(actual.camera.pitch == expected.camera.pitch);
    CHECK(actual.camera.yaw == expected.camera.yaw);
    CHECK(actual.camera.roll == expected.camera.roll);
    CHECK(actual.horizontalFov == expected.horizontalFov);
    CHECK(actual.aspect == expected.aspect);
    for (usize seat = 0; seat < actual.players.size(); ++seat) {
        REQUIRE(actual.players[seat].has_value() == expected.players[seat].has_value());
        if (actual.players[seat]) {
            CHECK(actual.players[seat]->grant == expected.players[seat]->grant);
            CHECK(actual.players[seat]->continuity == expected.players[seat]->continuity);
            CHECK(actual.players[seat]->position == expected.players[seat]->position);
            CHECK(actual.players[seat]->yaw == expected.players[seat]->yaw);
        }
    }
}

TEST_CASE("motion packets round trip every seat mask with explicit little endian fields",
          "[netplay][snapshot]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        auto snapshot = state(0x0123456789ABCDEFULL);
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                snapshot.players[seat].reset();
            }
        }
        const auto bytes = encode(snapshot);
        CHECK(bytes.size() == MotionPacket::kHeaderBytes + static_cast<usize>(std::popcount(mask)) *
                                                               MotionPacket::kSeatBytes);
        CHECK(bytes.size() <= PacketTransport::kMaxPacketBytes);
        CHECK(bytes[0] == 'G');
        CHECK(bytes[1] == 'D');
        CHECK(bytes[2] == 'L');
        CHECK(bytes[3] == 'M');
        CHECK(bytes[4] == 1);
        CHECK(bytes[5] == 0);
        CHECK(bytes[6] == mask);
        CHECK(bytes[7] == 0);
        CHECK(bytes[8] == 9);
        CHECK(bytes[16] == 0xEF);
        CHECK(bytes[23] == 0x01);
        const auto decoded = MotionPacket::decode(bytes);
        REQUIRE(decoded);
        equalState(*decoded, snapshot);
    }
}

TEST_CASE("motion decoder rejects truncated oversized reserved and nonfinite packets",
          "[netplay][snapshot]") {
    const auto valid = encode(state());
    for (usize size = 0; size < valid.size(); ++size) {
        CHECK_FALSE(MotionPacket::decode(std::span(valid).first(size)));
    }
    auto bytes = valid;
    bytes.push_back(0);
    CHECK_FALSE(MotionPacket::decode(bytes));
    for (const usize offset : {0U, 4U, 5U, 6U, 7U}) {
        bytes = valid;
        bytes[offset] = 255;
        CHECK_FALSE(MotionPacket::decode(bytes));
    }
    // Camera xyz/angles/FOV/aspect and the first player's xyz/yaw, in wire order.
    for (const usize offset : {28U, 32U, 36U, 40U, 44U, 48U, 52U, 56U, 68U, 72U, 76U, 80U}) {
        for (const u32 invalid : {0x7FC00000U, 0x7F800000U, 0xFF800000U}) {
            bytes = valid;
            for (usize i = 0; i < 4; ++i) {
                bytes[offset + i] = static_cast<u8>(invalid >> (i * 8));
            }
            CHECK_FALSE(MotionPacket::decode(bytes));
        }
    }
    // No uninitialized epoch, continuity or occupant identity.
    for (const usize offset : {8U, 24U, 60U, 64U}) {
        bytes = valid;
        for (usize i = 0; i < 4; ++i) {
            bytes[offset + i] = 0;
        }
        CHECK_FALSE(MotionPacket::decode(bytes));
    }
    auto invalid = state();
    invalid.camera.position.x = 1'000'001;
    CHECK_FALSE(MotionPacket::encode(invalid));
    invalid = state();
    invalid.players[3]->yaw = 999;
    CHECK_FALSE(MotionPacket::encode(invalid));
    invalid = state();
    invalid.horizontalFov = 0;
    CHECK_FALSE(MotionPacket::encode(invalid));
    invalid = state();
    invalid.aspect = 0;
    CHECK_FALSE(MotionPacket::encode(invalid));
}

TEST_CASE("snapshot admission binds the host and epoch without rollback on reordering",
          "[netplay][snapshot]") {
    SnapshotPlayback playback;
    auto bytes = encode(state(12));
    CHECK_FALSE(playback.sample(0));
    CHECK(playback.receive(1, bytes) == Admission::WrongHost);
    CHECK_FALSE(playback.begin(0, 9));
    CHECK_FALSE(playback.begin(1, 0));
    REQUIRE(playback.begin(1, 9));
    CHECK(playback.receive(2, bytes) == Admission::WrongHost);
    CHECK(playback.receive(1, {}) == Admission::Invalid);
    CHECK(playback.receive(1, bytes) == Admission::Accepted);
    CHECK(playback.receive(1, bytes) == Admission::Stale);
    CHECK(playback.receive(1, encode(state(11))) == Admission::Stale);
    auto future = state(13);
    future.epoch = 10;
    CHECK(playback.receive(1, encode(future)) == Admission::WrongEpoch);
    REQUIRE(playback.latest());
    CHECK(playback.latest()->tick == 12);
    CHECK_FALSE(playback.begin(2, 10));
    CHECK_FALSE(playback.begin(1, 9));
    REQUIRE(playback.begin(1, 10));
    CHECK(playback.size() == 0);
    CHECK_FALSE(playback.latest());
    CHECK_FALSE(playback.sample(12));
    CHECK(playback.receive(1, bytes) == Admission::WrongEpoch);
    CHECK(playback.receive(1, encode(future)) == Admission::Accepted);
    playback.clear();
    CHECK(playback.receive(1, encode(future)) == Admission::WrongHost);
    CHECK(playback.begin(2, 1)); // an explicitly new session may restart numbering
}

TEST_CASE("client interpolates player and host camera together and holds during packet loss",
          "[netplay][snapshot]") {
    SnapshotPlayback playback;
    REQUIRE(playback.begin(1, 9));
    auto first = state(10);
    first.players[0]->yaw = 3.1f;
    first.camera.yaw = 3.1f;
    auto second = first;
    second.tick = 14;
    second.players[0]->position.x += 8;
    second.players[0]->yaw = -3.1f;
    second.camera.position.x += 8;
    second.camera.yaw = -3.1f;
    second.horizontalFov += 0.2f;
    REQUIRE(playback.receive(1, encode(first)) == Admission::Accepted);
    REQUIRE(playback.receive(1, encode(second)) == Admission::Accepted);
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    CHECK(middle->players[0]->position.x == Approx(first.players[0]->position.x + 4));
    CHECK(std::abs(middle->players[0]->yaw) == Approx(kPi));
    CHECK(middle->camera.position.x == Approx(first.camera.position.x + 4));
    CHECK(std::abs(middle->camera.yaw) == Approx(kPi));
    CHECK(middle->horizontalFov == Approx(first.horizontalFov + 0.1f));
    REQUIRE(playback.sample(10, 0.5f));
    CHECK(playback.sample(10, 0.5f)->players[0]->position.x == Approx(1));
    equalState(*playback.sample(0), first);
    equalState(*playback.sample(1000), second); // no unbounded extrapolation
    equalState(*playback.sample(14), second);
    for (const f32 invalid : {-1.0f, 1.0f, std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(playback.sample(12, invalid));
    }
}

TEST_CASE("loss cannot blend across teleports camera cuts respawns or empty seats",
          "[netplay][snapshot]") {
    SnapshotPlayback playback;
    REQUIRE(playback.begin(1, 9));
    const auto first = state(5);
    auto afterCut = first;
    afterCut.tick = 15; // the snapshot at the instant of teleport was lost
    afterCut.players[0]->position = {200, 300, 400};
    ++afterCut.players[0]->continuity;
    afterCut.players[1]->position = {-200, -300, -400};
    ++afterCut.players[1]->grant;
    afterCut.players[2].reset();
    afterCut.camera.position = {400, 500, 600};
    ++afterCut.cameraContinuity;
    REQUIRE(playback.receive(1, encode(first)) == Admission::Accepted);
    REQUIRE(playback.receive(1, encode(afterCut)) == Admission::Accepted);
    equalState(*playback.sample(14, 0.9f), first);
    equalState(*playback.sample(15), afterCut);
    auto respawn = afterCut;
    respawn.tick = 18;
    respawn.players[2] = SeatMotion{9, 1, {20, 0, 10}, 0};
    REQUIRE(playback.receive(1, encode(respawn)) == Admission::Accepted);
    CHECK_FALSE(playback.sample(17)->players[2]);
    REQUIRE(playback.sample(18)->players[2]);
    CHECK(playback.sample(18)->players[2]->grant == 9);
}

TEST_CASE("snapshot history stays bounded and sampling retains high tick precision",
          "[netplay][snapshot]") {
    SnapshotPlayback playback;
    REQUIRE(playback.begin(1, 9));
    constexpr u64 kStart = std::numeric_limits<u64>::max() - 1000;
    for (u64 tick = 0; tick < 100; ++tick) {
        auto snapshot = state(kStart + tick * 2);
        snapshot.players[0]->position.x = static_cast<f32>(tick * 2);
        REQUIRE(playback.receive(1, encode(snapshot)) == Admission::Accepted);
        CHECK(playback.size() <= SnapshotPlayback::kHistory);
    }
    CHECK(playback.size() == SnapshotPlayback::kHistory);
    CHECK(playback.sample(0)->tick == kStart + 136);
    CHECK(playback.sample(kStart + 195, 0.5f)->players[0]->position.x == Approx(195.5f));
    CHECK(playback.sample(std::numeric_limits<u64>::max())->tick == kStart + 198);
}
} // namespace
