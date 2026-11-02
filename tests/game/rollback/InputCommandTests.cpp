#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/rollback/InputCommand.h"

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
    result.aimDirection = Vec3{1, 0, 0};
    result.heldButtons = static_cast<u32>(CommandHeld::Attack);
    result.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    return result;
}

TEST_CASE("steadied commands round analogue jitter away and stay valid", "[netplay][protocol]") {
    InputCommand first = sample();
    first.direction = glm::normalize(Vec2{0.6f, 0.8f});
    first.magnitude = 0.731f;
    const auto aimAt = [](f32 degrees) {
        const f32 radians = glm::radians(degrees);
        return Vec3{std::sin(radians), 0, std::cos(radians)};
    };
    first.aimDirection = aimAt(17.0f);
    InputCommand second = first;
    // A stick wobbling by a third of a degree and a cursor drifting a fifth of a degree
    // across a moving character are the same command once steadied.
    second.direction = glm::normalize(Vec2{0.605f, 0.797f});
    second.magnitude = 0.734f;
    second.aimDirection = aimAt(17.2f);
    const InputCommand a = first.steadied();
    const InputCommand b = second.steadied();
    CHECK(a.valid());
    CHECK(a.direction == b.direction);
    CHECK(a.magnitude == b.magnitude);
    CHECK(a.aimDirection == b.aimDirection);
    CHECK(a.heldButtons == first.heldButtons);
    CHECK(a.tick == first.tick);
    CHECK(a.magnitude == Catch::Approx(0.734375f));
    CHECK(glm::length(a.direction) == Catch::Approx(1.0f));
    REQUIRE(a.aimDirection);
    CHECK(glm::length(*a.aimDirection) == Catch::Approx(1.0f));
    CHECK(a.aimDirection->y == 0);
    // A quarter turn of aim is a different command; so is letting the stick go.
    InputCommand turned = first;
    turned.aimDirection = Vec3{1, 0, 0};
    CHECK(turned.steadied().aimDirection != a.aimDirection);
    InputCommand released = first;
    released.magnitude = 0.004f;
    CHECK(released.steadied().magnitude == 0);
    CHECK(released.steadied().direction == Vec2{0});
    CHECK(released.steadied().valid());
    // The cursor dead zone's zero aim and an absent aim survive untouched.
    InputCommand retained = first;
    retained.aimDirection = Vec3{0};
    CHECK(retained.steadied().aimDirection == Vec3{0});
    InputCommand stick = first;
    stick.aimDirection.reset();
    CHECK_FALSE(stick.steadied().aimDirection);
}

TEST_CASE("console input has bounded mutating text and quantized held flight",
          "[netplay][protocol][console-input]") {
    auto command = sample();
    command.console = ConsoleAction{1, "get gold 50"};
    command.consoleFlight = {0.33f, -1, 0.25f};
    command = command.steadied();
    const auto encoded = InputPacket::encode(std::array{command});
    REQUIRE(encoded);
    const auto decoded = InputPacket::decode(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->front().console == command.console);
    CHECK(decoded->front().consoleFlight == command.consoleFlight);
    CHECK(encoded->size() == InputPacket::kHeaderBytes + InputPacket::commandBytes(command));
    for (usize cut = 0; cut < encoded->size(); ++cut) {
        CHECK_FALSE(InputPacket::decode(std::span(*encoded).first(cut)));
    }
    auto invalidFlight = *encoded;
    invalidFlight[InputPacket::kHeaderBytes + 56] = 128;
    CHECK_FALSE(InputPacket::decode(invalidFlight));
    for (const auto* line : {"", "get info", "GET INFO enemy#1", "help", "console_permissions 1",
                             "god\nget gold 5", "get\tgold 5", "freecam", "list items"}) {
        command.console = ConsoleAction{0, line};
        CHECK_FALSE(command.valid());
        CHECK_FALSE(InputPacket::encode(std::array{command}));
    }
    command.console = ConsoleAction{4, "god"};
    CHECK_FALSE(command.valid());
    command.console = ConsoleAction{0, "spawn " + std::string(155, 'a')};
    CHECK_FALSE(command.valid());
    command.console = ConsoleAction{0, "spawn " + std::string(154, 'a')};
    CHECK(command.valid());
    std::vector<InputCommand> crowded(InputPacket::kMaxCommands, command);
    CHECK_FALSE(InputPacket::encode(crowded));
}

TEST_CASE("input packets have a fixed endian and version independent of C++ layout",
          "[netplay][protocol]") {
    const std::array commands{sample()};
    const auto encoded = InputPacket::encode(commands);
    REQUIRE(encoded);
    const std::vector<u8> expected{
        0x47, 0x44, 0x4c, 0x49, 6,    0,    1,    0,    // header
        1,    0,    0,    0,    0,    0,    0,    0,    // epoch
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // tick
        4,    3,    2,    1,    2,    1,    0,    0,    // grant, seat, aim, topic/revision
        0,    0,    0,    0,    0,    0,    0x80, 0x3f, 0, 0, 0, 0x3f, // direction, magnitude
        0,    0,    0x80, 0x3f, 0,    0,    0,    0,    0, 0, 0, 0,    // aim
        1,    0,    0,    0,    1,    0,    0,    0,                   // held, pressed
        0,    0,    0,    0};                                          // flight, absent console
    CHECK(*encoded == expected);
    const auto decoded = InputPacket::decode(expected);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->front().tick == commands[0].tick);
    CHECK(decoded->front().aimDirection == commands[0].aimDirection);
    CHECK(decoded->front().grant == commands[0].grant);
    CHECK(InputPacket::encode(*decoded) == encoded);
}

TEST_CASE("input packets keep the byte layout of console text and held flight",
          "[netplay][protocol]") {
    InputCommand command;
    command.epoch = 0x0102030405060708ULL;
    command.tick = 9;
    command.grant = 7;
    command.seat = 1;
    command.direction = {-1, 0};
    command.magnitude = 1;
    command.hintTopic = 2;
    command.hintRevision = 5;
    command.consoleFlight = {-1, 0, 1};
    command.console = ConsoleAction{3, "god"};
    const std::array commands{sample(), command};
    const auto encoded = InputPacket::encode(commands);
    REQUIRE(encoded);
    constexpr std::string_view kPacket = "47444c49"         // magic
                                         "0600"             // version
                                         "02"               // commands
                                         "00"               // zero
                                         "0100000000000000" // first: epoch
                                         "8877665544332211" // tick
                                         "04030201"         // grant
                                         "02010000"         // seat, aim flag, hint topic, revision
                                         "000000000000803f0000003f" // direction, magnitude
                                         "0000803f0000000000000000" // aim
                                         "0100000001000000"         // held, pressed
                                         "000000"                   // flight
                                         "00"                       // console length
                                         "0807060504030201"         // second: epoch
                                         "0900000000000000"         // tick
                                         "07000000"                 // grant
                                         "01000205" // seat, aim flag, hint topic, revision
                                         "000080bf000000000000803f" // direction, magnitude
                                         "000000000000000000000000" // aim
                                         "0000000000000000"         // held, pressed
                                         "81007f"                   // flight
                                         "03"                       // console length
                                         "03"                       // console seat
                                         "676f64";                  // "god"
    CHECK(test::hexBytes(*encoded) == kPacket);
    const auto decoded = InputPacket::decode(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    CHECK(decoded->back().console == command.console);
    CHECK(decoded->back().consoleFlight == command.consoleFlight);
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
    auto legacy = *bytes;
    legacy[4] = 1; // absolute aim points must never be interpreted as directions
    CHECK_FALSE(InputPacket::decode(legacy));
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
            (*command.aimDirection)[component] = bad;
            rejects(command);
        }
    }
    auto command = good;
    for (const Vec3 bad : {Vec3{0.5f, 0, 0}, Vec3{1, 0, 1}, Vec3{0, 1, 0}}) {
        command.aimDirection = bad;
        rejects(command);
    }
    command.aimDirection = Vec3{0};
    CHECK(command.valid());
    command = good;
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

TEST_CASE("potion throws use the held wire domain and reject the retired press encoding",
          "[netplay][protocol][potion-input]") {
    auto command = sample();
    command.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
    command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    const auto bytes = InputPacket::encode(std::array{command});
    REQUIRE(bytes);
    const auto decoded = InputPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->front().held(CommandHeld::ThrowPotion));
    CHECK(decoded->front().pressed(CommandPress::UsePotion));
    auto legacyVersion = *bytes;
    legacyVersion[4] = 2;
    CHECK_FALSE(InputPacket::decode(legacyVersion));
    command.pressedButtons = 1U << 1U;
    CHECK_FALSE(command.valid());
    CHECK_FALSE(InputPacket::encode(std::array{command}));
    auto legacyPress = *bytes;
    legacyPress[InputPacket::kHeaderBytes + InputPacket::kCommandBytes - 8] = 2;
    CHECK_FALSE(InputPacket::decode(legacyPress));
}

TEST_CASE("redundant input batches stay bounded and fail atomically", "[netplay][protocol]") {
    std::vector<InputCommand> batch(InputPacket::kMaxCommands, sample());
    for (usize i = 0; i < batch.size(); ++i) {
        batch[i].seat = static_cast<u8>(i % InputCommand::kSeats);
        batch[i].tick = i / InputCommand::kSeats;
        batch[i].aimDirection.reset();
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
