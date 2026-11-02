#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/rollback/InputJournal.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr u64 kEpoch = 5;
constexpr RollbackInputs::Owners kOwners{1, 2, 3, 0};
constexpr std::array<u32, 4> kGrants{21, 22, 23, 0};

InputCommand command(u64 tick, u8 seat, f32 x = 0, u32 pressed = 0) {
    InputCommand result;
    result.epoch = kEpoch;
    result.tick = tick;
    result.grant = kGrants[seat];
    result.seat = seat;
    result.direction = {x, 0};
    result.magnitude = x == 0 ? 0.0f : 1.0f;
    result.pressedButtons = pressed;
    return result;
}

JournalFrame frame(u64 tick) {
    JournalFrame result;
    // Seat 0 changes every ten ticks, seat 1 never, seat 2 presses every other tick.
    result[0] = command(tick, 0, (tick / 10) % 2 == 0 ? 1.0f : -1.0f);
    result[1] = command(tick, 1);
    result[2] = command(tick, 2, 0, tick % 2 == 0 ? static_cast<u32>(CommandPress::Attack) : 0);
    return result;
}

EpochInputJournal journalOf(u64 ticks) {
    EpochInputJournal journal;
    REQUIRE(journal.open(kEpoch, kOwners));
    for (u64 tick = 1; tick <= ticks; ++tick) {
        REQUIRE(journal.append(tick, frame(tick)));
    }
    return journal;
}

TEST_CASE("a journal takes frames in tick order under the epoch's owners", "[netplay][journal]") {
    EpochInputJournal journal;
    CHECK_FALSE(journal.opened());
    CHECK_FALSE(journal.open(0, kOwners));
    CHECK_FALSE(journal.open(kEpoch, {0, 0, 0, 0}));
    REQUIRE(journal.open(kEpoch, kOwners));
    CHECK(journal.through() == 0);
    REQUIRE(journal.append(1, frame(1)));
    CHECK_FALSE(journal.append(3, frame(3))); // tick two first
    CHECK_FALSE(journal.append(2, frame(3))); // the frame must carry its tick
    auto foreign = frame(2);
    foreign[0].epoch = kEpoch + 1;
    CHECK_FALSE(journal.append(2, foreign));
    auto unoccupied = frame(2);
    unoccupied[3].epoch = 77; // an unoccupied seat's command is not inspected
    REQUIRE(journal.append(2, unoccupied));
    CHECK(journal.through() == 2);
    REQUIRE(journal.frame(2) != nullptr);
    CHECK(journal.frame(2)->at(0).tick == 2);
    CHECK(journal.frame(3) == nullptr);
    CHECK(journal.frame(0) == nullptr);
    journal.clear();
    CHECK_FALSE(journal.opened());
}

TEST_CASE("journals retain variable console actions and flight for a full epoch rebuild",
          "[netplay][journal][console-input]") {
    EpochInputJournal journal;
    REQUIRE(journal.open(kEpoch, kOwners));
    std::vector<InputCommand> loose;
    for (u64 tick = 1; tick <= 40; ++tick) {
        auto values = frame(tick);
        values[0].console =
            ConsoleAction{1, "spawn " + std::string(153, 'a') + static_cast<char>('a' + tick % 26)};
        values[1].consoleFlight = Vec3{0, -1, 1};
        REQUIRE(journal.append(tick, values));
        loose.push_back(values[0]);
    }
    for (const auto& chunks : {JournalPacket::chunks(journal, 1, 40, true),
                               JournalPacket::tailChunks(kEpoch, loose, true)}) {
        REQUIRE(chunks.size() > 1);
        usize carried = 0;
        for (const auto& chunk : chunks) {
            const auto encoded = JournalPacket::encode(chunk);
            REQUIRE(encoded);
            CHECK(encoded->size() <= JournalPacket::kMaxBytes);
            const auto decoded = JournalPacket::decode(*encoded);
            REQUIRE(decoded);
            for (const auto& got : decoded->commands) {
                const auto& expected = (*journal.frame(got.tick))[got.seat];
                CHECK(got.console == expected.console);
                CHECK(got.consoleFlight == expected.consoleFlight);
                carried += got.console ? 1U : 0U;
            }
        }
        CHECK(carried == 40);
        CHECK(chunks.back().last);
    }
    auto foreign = frame(41);
    foreign[1].console = ConsoleAction{1, "god"};
    CHECK_FALSE(journal.append(41, foreign));
}

TEST_CASE("journal chunks pack runs, fit the wire and round-trip exactly", "[netplay][journal]") {
    const auto journal = journalOf(1000);
    const auto chunks = JournalPacket::chunks(journal, 1, 1000, true);
    REQUIRE_FALSE(chunks.empty());
    u64 next = 1;
    std::vector<JournalFrame> rebuilt;
    for (usize index = 0; index < chunks.size(); ++index) {
        const auto& chunk = chunks[index];
        CHECK(chunk.firstTick == next);
        CHECK(chunk.seats == 0b0111);
        CHECK(chunk.last == (index + 1 == chunks.size()));
        const auto bytes = JournalPacket::encode(chunk);
        REQUIRE(bytes);
        CHECK(bytes->size() <= JournalPacket::kMaxBytes);
        CHECK(JournalPacket::recognizes(*bytes));
        const auto decoded = JournalPacket::decode(*bytes);
        REQUIRE(decoded);
        CHECK(decoded->commands.size() == chunk.commands.size());
        CHECK(decoded->last == chunk.last);
        for (u16 i = 0; i < decoded->count; ++i) {
            JournalFrame frame;
            for (u8 seat = 0; seat < 3; ++seat) {
                frame[seat] = decoded->commands[static_cast<usize>(seat) * decoded->count + i];
            }
            rebuilt.push_back(frame);
        }
        next += chunk.count;
    }
    CHECK(next == 1001);
    REQUIRE(rebuilt.size() == 1000);
    for (u64 tick = 1; tick <= 1000; ++tick) {
        const auto& expected = *journal.frame(tick);
        for (u8 seat = 0; seat < 3; ++seat) {
            const auto& got = rebuilt[tick - 1][seat];
            CHECK(got.tick == tick);
            CHECK(got.seat == seat);
            CHECK(got.direction == expected[seat].direction);
            CHECK(got.magnitude == expected[seat].magnitude);
            CHECK(got.pressedButtons == expected[seat].pressedButtons);
            CHECK(got.grant == expected[seat].grant);
        }
    }
    // Seat 2 changes every tick, so about 27 ticks fill a chunk; the whole second of
    // steady input in a two-seat journal would fit one.
    CHECK(chunks.size() > 30);
    CHECK(JournalPacket::chunks(journal, 1, 1001, true).empty());
    CHECK(JournalPacket::chunks(journal, 0, 10, true).empty());
    const auto tail = JournalPacket::chunks(journal, 990, 1000, false);
    REQUIRE(tail.size() == 1);
    CHECK_FALSE(tail.front().last);
    CHECK(tail.front().count == 11);
}

TEST_CASE("journal chunks keep their byte layout", "[netplay][journal]") {
    JournalPacket::Chunk chunk;
    chunk.epoch = 0x1122334455667788ULL;
    chunk.firstTick = 0x0102030405060708ULL;
    chunk.count = 3;
    chunk.seats = 0b0101;
    chunk.last = true;
    for (const u8 seat : {u8{0}, u8{2}}) {
        for (u16 i = 0; i < chunk.count; ++i) {
            InputCommand command;
            command.epoch = chunk.epoch;
            command.tick = chunk.firstTick + i;
            command.grant = 0x0a0b0c0dU + seat;
            command.seat = seat;
            if (seat == 0) {
                command.direction = {i < 2 ? 1.0f : -1.0f, 0};
                command.magnitude = 1;
                command.heldButtons = static_cast<u32>(CommandHeld::Attack);
            } else {
                command.aimDirection = Vec3{0, 0, 1};
                command.hintTopic = 4;
                command.hintRevision = 9;
                command.consoleFlight = {1, -1, 0};
                if (i == 0) {
                    command.console = ConsoleAction{2, "god"};
                }
            }
            chunk.commands.push_back(command);
        }
    }
    const auto bytes = JournalPacket::encode(chunk);
    REQUIRE(bytes);
    constexpr std::string_view kChunk = "47444c4a"                   // magic
                                        "02010500"                   // version, last, seats, zero
                                        "8877665544332211"           // epoch
                                        "0807060504030201"           // first tick
                                        "0300"                       // count
                                        "0000"                       // zero
                                        "0d0c0b0a"                   // seat 0, ticks 0 and 1: grant
                                        "0000803f000000000000803f"   // direction, magnitude
                                        "00000000000000000000000000" // aim flag, aim
                                        "0100000000000000"           // held, pressed
                                        "0000"                       // hint topic, revision
                                        "000000"                     // flight
                                        "00"                         // console length
                                        "0200"                       // run
                                        "0d0c0b0a"                   // seat 0, tick 2: grant
                                        "000080bf000000000000803f"   // direction, magnitude
                                        "00000000000000000000000000" // aim flag, aim
                                        "0100000000000000"           // held, pressed
                                        "0000"                       // hint topic, revision
                                        "000000"                     // flight
                                        "00"                         // console length
                                        "0100"                       // run
                                        "0f0c0b0a"                   // seat 2, tick 0: grant
                                        "000000000000000000000000"   // direction, magnitude
                                        "0100000000000000000000803f" // aim flag, aim
                                        "0000000000000000"           // held, pressed
                                        "0409"                       // hint topic, revision
                                        "7f8100"                     // flight
                                        "03"                         // console length
                                        "02"                         // console seat
                                        "676f64"                     // "god"
                                        "0100"                       // run
                                        "0f0c0b0a"                   // seat 2, ticks 1 and 2: grant
                                        "000000000000000000000000"   // direction, magnitude
                                        "0100000000000000000000803f" // aim flag, aim
                                        "0000000000000000"           // held, pressed
                                        "0409"                       // hint topic, revision
                                        "7f8100"                     // flight
                                        "00"                         // console length
                                        "0200";                      // run
    CHECK(test::hexBytes(*bytes) == kChunk);
    const auto decoded = JournalPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->commands.size() == chunk.commands.size());
    CHECK(JournalPacket::encode(*decoded) == bytes);
}

TEST_CASE("a steady journal packs a long stretch into one chunk", "[netplay][journal]") {
    EpochInputJournal journal;
    REQUIRE(journal.open(kEpoch, {1, 2, 0, 0}));
    for (u64 tick = 1; tick <= 3000; ++tick) {
        JournalFrame frame;
        frame[0] = command(tick, 0, 1.0f);
        frame[1] = command(tick, 1);
        REQUIRE(journal.append(tick, frame));
    }
    const auto chunks = JournalPacket::chunks(journal, 1, 3000, true);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks.front().count == 3000);
    const auto bytes = JournalPacket::encode(chunks.front());
    REQUIRE(bytes);
    CHECK(bytes->size() == JournalPacket::kHeaderBytes + 2 * JournalPacket::kRunBytes);
    const auto decoded = JournalPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->commands.size() == 6000);
    CHECK(decoded->commands[2999].tick == 3000);
    CHECK(decoded->commands[3000].seat == 1);
}

TEST_CASE("journal packets reject malformed bytes and impossible chunks", "[netplay][journal]") {
    const auto journal = journalOf(20);
    const auto chunks = JournalPacket::chunks(journal, 1, 20, true);
    REQUIRE(chunks.size() == 1);
    const auto bytes = *JournalPacket::encode(chunks.front());
    for (usize cut = 0; cut < bytes.size(); cut += 7) {
        CHECK_FALSE(JournalPacket::decode(std::span(bytes).first(cut)));
    }
    auto version = bytes;
    version[4] = 1;
    CHECK_FALSE(JournalPacket::decode(version));
    auto flags = bytes;
    flags[5] = 0x80;
    CHECK_FALSE(JournalPacket::decode(flags));
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(JournalPacket::decode(trailing));
    auto zeroRun = bytes;
    zeroRun[JournalPacket::kHeaderBytes + JournalPacket::kRunBytes - 2] = 0;
    zeroRun[JournalPacket::kHeaderBytes + JournalPacket::kRunBytes - 1] = 0;
    CHECK_FALSE(JournalPacket::decode(zeroRun));
    const JournalPacket::Chunk empty;
    CHECK_FALSE(JournalPacket::encode(empty));
    auto mismatched = chunks.front();
    mismatched.commands.pop_back();
    CHECK_FALSE(mismatched.valid());
    CHECK_FALSE(JournalPacket::encode(mismatched));
    auto wrongTick = chunks.front();
    wrongTick.commands[3].tick = 99;
    CHECK_FALSE(JournalPacket::encode(wrongTick));
    std::vector<u8> noise(JournalPacket::kMaxBytes + 1, 0);
    CHECK_FALSE(JournalPacket::decode(noise));
    CHECK_FALSE(JournalPacket::recognizes(std::span(noise).first(3)));
}

TEST_CASE("tail chunks cut loose commands at seat and tick gaps and fit the wire",
          "[netplay][journal]") {
    std::vector<InputCommand> loose;
    for (u64 tick = 31; tick <= 40; ++tick) {
        loose.push_back(command(tick, 0, 1.0f));
    }
    for (u64 tick = 31; tick <= 35; ++tick) {
        loose.push_back(command(tick, 2));
    }
    loose.push_back(command(38, 2)); // a gap at 36 and 37
    auto foreign = command(50, 1);
    foreign.epoch = 99; // another epoch's command is dropped
    loose.push_back(foreign);
    const auto chunks = JournalPacket::tailChunks(kEpoch, loose, true);
    REQUIRE(chunks.size() == 3);
    CHECK(chunks[0].seats == 1);
    CHECK(chunks[0].firstTick == 31);
    CHECK(chunks[0].count == 10);
    CHECK_FALSE(chunks[0].last);
    CHECK(chunks[1].seats == 4);
    CHECK(chunks[1].firstTick == 31);
    CHECK(chunks[1].count == 5);
    CHECK(chunks[2].seats == 4);
    CHECK(chunks[2].firstTick == 38);
    CHECK(chunks[2].count == 1);
    CHECK(chunks[2].last);
    for (const auto& chunk : chunks) {
        const auto bytes = JournalPacket::encode(chunk);
        REQUIRE(bytes);
        const auto decoded = JournalPacket::decode(*bytes);
        REQUIRE(decoded);
        CHECK(decoded->commands.size() == chunk.commands.size());
    }
    // One seat changing every tick splits at the wire's run capacity.
    std::vector<InputCommand> busy;
    for (u64 tick = 1; tick <= 100; ++tick) {
        busy.push_back(command(tick, 1, tick % 2 == 0 ? 1.0f : -1.0f));
    }
    const auto pieces = JournalPacket::tailChunks(kEpoch, busy, false);
    CHECK(pieces.size() == 4);
    for (const auto& piece : pieces) {
        const auto bytes = JournalPacket::encode(piece);
        REQUIRE(bytes);
        CHECK(bytes->size() <= JournalPacket::kMaxBytes);
        CHECK_FALSE(piece.last);
    }
    CHECK(JournalPacket::tailChunks(kEpoch, {}, true).empty());
}

} // namespace
