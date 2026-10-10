#include <array>
#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/InputCommand.h"

namespace {
using namespace gdl;
using namespace gdl::game;

InputCommand sample() {
    InputCommand result;
    result.epoch = 1;
    result.tick = 0x1122334455667788ULL;
    result.grant = 0x01020304;
    result.seat = 2;
    result.direction = {0, 1};
    result.magnitude = 0.5f;
    result.aimPoint = Vec3{2, -3, 4};
    result.heldButtons = static_cast<u32>(CommandHeld::Attack);
    result.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    return result;
}

TEST_CASE("input packets have a fixed endian and version independent of C++ layout",
          "[netplay][protocol]") {
    const std::array commands{sample()};
    const auto encoded = InputPacket::encode(commands);
    REQUIRE(encoded);
    const std::vector<u8> expected{
        0x47, 0x44, 0x4c, 0x49, 1,    0,    1,    0,    // header
        1,    0,    0,    0,    0,    0,    0,    0,    // epoch
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // tick
        4,    3,    2,    1,    2,    1,    0,    0,    // grant, seat, aim, reserved
        0,    0,    0,    0,    0,    0,    0x80, 0x3f, 0, 0, 0,    0x3f, // direction, magnitude
        0,    0,    0,    0x40, 0,    0,    0x40, 0xc0, 0, 0, 0x80, 0x40, // aim
        1,    0,    0,    0,    1,    0,    0,    0};                     // held, pressed
    CHECK(*encoded == expected);
    const auto decoded = InputPacket::decode(expected);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->front().tick == commands[0].tick);
    CHECK(decoded->front().aimPoint == commands[0].aimPoint);
    CHECK(decoded->front().grant == commands[0].grant);
    CHECK(InputPacket::encode(*decoded) == encoded);
}

TEST_CASE("input packets reject truncation trailing bytes and unsupported protocol fields",
          "[netplay][protocol]") {
    const auto bytes = InputPacket::encode(std::array{sample()});
    REQUIRE(bytes);
    for (usize length = 0; length < bytes->size(); ++length) {
        CAPTURE(length);
        CHECK_FALSE(InputPacket::decode(std::span(*bytes).first(length)));
    }
    auto appended = *bytes;
    appended.push_back(0);
    CHECK_FALSE(InputPacket::decode(appended));
    for (const usize offset : {0U, 4U, 5U, 6U, 7U, 28U, 29U, 30U, 31U, 59U, 63U}) {
        CAPTURE(offset);
        auto corrupt = *bytes;
        corrupt[offset] = 0xff;
        CHECK_FALSE(InputPacket::decode(corrupt));
    }
    auto missingAim = *bytes;
    missingAim[29] = 0;
    CHECK_FALSE(InputPacket::decode(missingAim)); // aim payload must be absent too
}

TEST_CASE("input validation rejects impossible movement and nonfinite cursor targets",
          "[netplay][protocol]") {
    const auto good = sample();
    const auto rejects = [](const InputCommand& command) {
        CHECK_FALSE(command.valid());
        CHECK_FALSE(InputPacket::encode(std::array{command}));
    };
    for (const f32 bad : {-1.0f, 1.01f, std::numeric_limits<f32>::infinity(),
                          std::numeric_limits<f32>::quiet_NaN()}) {
        auto command = good;
        command.magnitude = bad;
        rejects(command);
    }
    for (const Vec2 bad : {Vec2{1, 1}, Vec2{0}, Vec2{std::numeric_limits<f32>::quiet_NaN(), 0}}) {
        auto command = good;
        command.direction = bad;
        rejects(command);
    }
    for (const f32 bad : {1'000'001.0f, std::numeric_limits<f32>::infinity(),
                          std::numeric_limits<f32>::quiet_NaN()}) {
        for (s32 component = 0; component < 3; ++component) {
            auto command = good;
            (*command.aimPoint)[component] = bad;
            rejects(command);
        }
    }
    auto command = good;
    command.epoch = 0;
    rejects(command);
    command = good;
    command.grant = 0;
    rejects(command);
    command = good;
    command.seat = 4;
    rejects(command);
    command = good;
    command.heldButtons = ~InputCommand::kHeldMask;
    rejects(command);
    command = good;
    command.pressedButtons = ~InputCommand::kPressMask;
    rejects(command);
}

TEST_CASE("redundant input batches stay bounded and fail atomically", "[netplay][protocol]") {
    std::vector<InputCommand> batch(InputPacket::kMaxCommands, sample());
    for (usize i = 0; i < batch.size(); ++i) {
        batch[i].seat = static_cast<u8>(i % InputCommand::kSeats);
        batch[i].tick = i / InputCommand::kSeats;
        batch[i].aimPoint.reset();
    }
    const auto encoded = InputPacket::encode(batch);
    REQUIRE(encoded);
    CHECK(encoded->size() == InputPacket::kMaxBytes);
    const auto decoded = InputPacket::decode(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->size() == batch.size());
    CHECK(InputPacket::encode(*decoded) == encoded);
    auto corrupt = *encoded;
    corrupt.back() = 0xff;
    CHECK_FALSE(InputPacket::decode(corrupt));
    batch.push_back(sample());
    CHECK_FALSE(InputPacket::encode(batch));
    CHECK_FALSE(InputPacket::encode({}));
    CHECK_FALSE(InputPacket::decode(std::vector<u8>(InputPacket::kMaxBytes + 1)));
}

TEST_CASE("single-bit input packet mutations cannot produce unchecked commands",
          "[netplay][protocol]") {
    const auto encoded = InputPacket::encode(std::array{sample()});
    REQUIRE(encoded);
    for (usize byte = 0; byte < encoded->size(); ++byte) {
        for (u32 bit = 0; bit < 8; ++bit) {
            auto corrupt = *encoded;
            corrupt[byte] ^= static_cast<u8>(1U << bit);
            if (const auto decoded = InputPacket::decode(corrupt)) {
                REQUIRE(decoded->size() == 1);
                CHECK(decoded->front().valid());
            }
        }
    }
}

TEST_CASE("outbound input history repeats four ticks for all locally owned seats",
          "[netplay][protocol]") {
    InputHistory history;
    std::array<InputCommand, 4> frame;
    for (usize seat = 0; seat < frame.size(); ++seat) {
        frame[seat] = sample();
        frame[seat].seat = static_cast<u8>(seat);
    }
    for (u64 tick = 0; tick < 30; ++tick) {
        for (auto& command : frame) {
            command.tick = tick;
        }
        REQUIRE(history.record(frame));
        CHECK(history.commands().size() <= InputPacket::kMaxCommands);
        REQUIRE(InputPacket::encode(history.commands()));
        for (const auto& command : history.commands()) {
            CHECK(command.tick <= tick);
            CHECK(tick - command.tick < InputHistory::kTicks);
        }
        if (tick >= 3) {
            CHECK(history.commands().size() == 16);
        }
    }
    frame[0].tick += 10; // a large gap cannot retain stale presses
    REQUIRE(history.record(std::span(frame).first(1)));
    CHECK(history.commands().size() == 1);
}

TEST_CASE("outbound history rejects malformed frames without changing pending redundancy",
          "[netplay][protocol]") {
    InputHistory history;
    auto command = sample();
    command.tick = 0;
    REQUIRE(history.record(std::array{command}));
    CHECK_FALSE(history.record(std::array{command})); // sampling twice for the same tick
    command.tick = 1;
    CHECK_FALSE(history.record(std::array{command, command})); // two commands for one seat
    auto other = command;
    other.seat = 1;
    other.epoch = 2;
    CHECK_FALSE(history.record(std::array{command, other}));
    other.epoch = 1;
    other.tick = 2;
    CHECK_FALSE(history.record(std::array{command, other}));
    CHECK_FALSE(history.record({}));
    REQUIRE(history.commands().size() == 1);
    CHECK(history.commands().front().tick == 0);
}

TEST_CASE("outbound history drops old epochs and revoked seats when the roster changes",
          "[netplay][protocol]") {
    InputHistory history;
    auto first = sample();
    first.tick = 0;
    auto second = first;
    second.seat = 3;
    REQUIRE(history.record(std::array{first, second}));
    first.tick = 1;
    ++first.grant;
    REQUIRE(history.record(std::array{first}));
    REQUIRE(history.commands().size() == 1);
    CHECK(history.commands().front().grant == first.grant);
    first.epoch = 2;
    first.tick = 0;
    REQUIRE(history.record(std::array{first}));
    REQUIRE(history.commands().size() == 1);
    CHECK(history.commands().front().epoch == 2);
    CHECK_FALSE(history.record(std::array{second}));
    history.clear();
    CHECK(history.commands().empty());
}
} // namespace
