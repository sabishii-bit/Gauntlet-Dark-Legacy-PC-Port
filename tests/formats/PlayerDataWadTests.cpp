#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "formats/PlayerDataWad.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

constexpr std::size_t kRecordSize = 0x180;

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(&bytes[offset], &value, 4);
}

void putF32(std::vector<std::uint8_t>& bytes, std::size_t offset, float value) {
    putU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

/** A wad with one PDAT record after the header and its directory at the end. */
std::vector<std::uint8_t> sampleWad() {
    const std::size_t record = 16;
    const std::size_t directory = record + kRecordSize;
    std::vector<std::uint8_t> bytes(directory + 16, 0);
    putU32(bytes, 0, static_cast<std::uint32_t>(directory));
    putU32(bytes, 4, 1);
    bytes[record] = 15; // effect count
    bytes[record + 2] = 14;
    putF32(bytes, record + 0x28, 600.0f);
    putF32(bytes, record + 0x2C, 999.0f);
    putF32(bytes, record + 0x30, 350.0f);
    putF32(bytes, record + 0x34, 750.0f);
    putF32(bytes, record + 0x38, 300.0f);
    putF32(bytes, record + 0x3C, 700.0f);
    putF32(bytes, record + 0x40, 100.0f);
    putF32(bytes, record + 0x44, 500.0f);
    putF32(bytes, record + 0x48, 5.0f);
    putF32(bytes, record + 0x4C, 1.5f);
    putF32(bytes, record + 0x50, 4.4f);
    putF32(bytes, record + 0x54, 2.5f);
    putF32(bytes, record + 0x58, 1.25f);
    putF32(bytes, record + 0x5C, -0.5f);
    putF32(bytes, record + 0x60, 0.5f);
    putF32(bytes, record + 0x64, 1.5f);
    bytes[directory] = 'T';
    bytes[directory + 1] = 'A';
    bytes[directory + 2] = 'D';
    bytes[directory + 3] = 'P';
    putU32(bytes, directory + 4, static_cast<std::uint32_t>(record));
    putU32(bytes, directory + 8, 1);
    putU32(bytes, directory + 12, 1);
    return bytes;
}

TEST_CASE("a class record parses from its wad", "[formats][pdata]") {
    const PlayerClassRecord record = parsePlayerDataWad(sampleWad());
    REQUIRE(record.effectCount == 15);
    REQUIRE(record.damageCount == 14);
    REQUIRE(record.fightMin == 600.0f);
    REQUIRE(record.fightMax == 999.0f);
    REQUIRE(record.speedMin == 350.0f);
    REQUIRE(record.magicMax == 500.0f);
    REQUIRE(record.height == 5.0f);
    REQUIRE(record.width == 1.5f);
    REQUIRE(record.attachY == 4.4f);
    REQUIRE(record.collisionY == 2.5f);
    REQUIRE(record.powerupTime == 1.25f);
    REQUIRE(record.weaponOffset == std::array<float, 3>{-0.5f, 0.5f, 1.5f});
}

TEST_CASE("damaged class wads are rejected", "[formats][pdata]") {
    std::vector<std::uint8_t> bytes = sampleWad();
    bytes.resize(8);
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    bytes[bytes.size() - 16] = 'X'; // not a PDAT section
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    putU32(bytes, bytes.size() - 12, 0x1000); // record beyond the file
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    putU32(bytes, 4, 100); // more directory entries than fit
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);
}

TEST_CASE("a class wad gives its moves: the effects they show and the strikes they make",
          "[formats][pdata]") {
    // One effect, two strikes, then the class record, then the directory of all three.
    constexpr std::size_t kEffect = 0x50;
    constexpr std::size_t kStrike = 0x58;
    const std::size_t effects = 16;
    const std::size_t strikes = effects + kEffect;
    const std::size_t record = strikes + 2 * kStrike;
    const std::size_t directory = record + kRecordSize;
    constexpr std::size_t kEntry = 16;
    std::vector<std::uint8_t> bytes(directory + 3 * kEntry, 0);
    putU32(bytes, 0, static_cast<std::uint32_t>(directory));
    putU32(bytes, 4, 3);
    const auto section = [&](std::size_t slot, std::string_view tag, std::size_t offset,
                             std::uint32_t count) {
        const std::size_t at = directory + slot * kEntry;
        for (std::size_t c = 0; c < 4; ++c) {
            bytes[at + c] = static_cast<std::uint8_t>(tag[3 - c]); // tags lie the other way round
        }
        putU32(bytes, at + 4, static_cast<std::uint32_t>(offset));
        putU32(bytes, at + 8, count);
        putU32(bytes, at + 12, count);
    };
    section(0, "SFXX", effects, 1);
    section(1, "DAMG", strikes, 2);
    section(2, "PDAT", record, 1);
    const auto put16 = [&](std::size_t at, std::int16_t value) {
        const auto raw = static_cast<std::uint16_t>(value);
        bytes[at] = static_cast<std::uint8_t>(raw & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>(raw >> 8);
    };
    put16(record, 1);     // effects
    put16(record + 2, 2); // strikes
    for (std::size_t move = 0; move < PlayerClassRecord::kMoveCount; ++move) {
        put16(record + 0x0C + move * 2, -1);
    }
    put16(record + 0x0C + std::size_t{5} * 2, 0); // turbo B starts at the first strike
    putU32(bytes, effects, 4);
    putU32(bytes, effects + 4, 0xFFFFFFFF);
    std::memcpy(&bytes[effects + 0x10], "WAR_POWERB", 10);
    std::memcpy(&bytes[effects + 0x20], "S_WARTURBOB", 11);
    putF32(bytes, effects + 0x38, 5.0f);
    putF32(bytes, effects + 0x48, 2.0f);
    put16(strikes, 4);
    putF32(bytes, strikes + 0x0C, 12.0f);
    putF32(bytes, strikes + 0x14, 0.5f);
    putF32(bytes, strikes + 0x24, -1.0f);
    putF32(bytes, strikes + 0x38, 50.0f);
    put16(strikes + 0x48, 0);
    put16(strikes + 0x4E, 1);
    put16(strikes + kStrike, 2);
    putF32(bytes, strikes + kStrike + 0x08, 10.0f);
    putF32(bytes, strikes + kStrike + 0x1C, 6.0f);
    putF32(bytes, strikes + kStrike + 0x34, 5.0f);
    putF32(bytes, strikes + kStrike + 0x38, -1.5f);
    putF32(bytes, strikes + kStrike + 0x3C, 30.0f);
    putF32(bytes, strikes + kStrike + 0x40, 40.0f);
    put16(strikes + kStrike + 0x4E, -1);
    put16(strikes + kStrike + 0x50, 9);

    const PlayerClassRecord parsed = parsePlayerDataWad(bytes);
    REQUIRE(PlayerClassRecord::kMoveNames[5] == "turboB");
    REQUIRE(parsed.moves[5] == 0);
    REQUIRE(parsed.moves[6] == -1);
    REQUIRE(parsed.effects.size() == 1);
    REQUIRE(parsed.effects[0].tree == "WAR_POWERB");
    REQUIRE(parsed.effects[0].sound == "S_WARTURBOB");
    REQUIRE(parsed.effects[0].next == -1);
    REQUIRE(parsed.effects[0].offset == std::array<float, 3>{0.0f, 5.0f, 0.0f});
    REQUIRE(parsed.effects[0].scale == 2.0f);
    REQUIRE(parsed.strikes.size() == 2);
    REQUIRE(parsed.strikes[0].type == 4);
    REQUIRE(parsed.strikes[0].radius == 12.0f);
    REQUIRE(parsed.strikes[0].delay == 0.5f);
    REQUIRE(parsed.strikes[0].amount == 50.0f);
    REQUIRE(parsed.strikes[0].next == 1);
    REQUIRE(parsed.strikes[1].type == 2);
    REQUIRE(parsed.strikes[1].hitRadius == 10.0f);
    REQUIRE(parsed.strikes[1].maxTime == 6.0f);
    REQUIRE(parsed.strikes[1].offset == std::array<float, 3>{0.0f, 0.0f, 5.0f});
    REQUIRE(parsed.strikes[1].amount == -1.5f);
    REQUIRE(parsed.strikes[1].speedMax == 40.0f);
    REQUIRE(parsed.strikes[1].startFrame == 9);
    // A wad with no such sections has no moves to give.
    REQUIRE(parsePlayerDataWad(sampleWad()).strikes.empty());
}

} // namespace
