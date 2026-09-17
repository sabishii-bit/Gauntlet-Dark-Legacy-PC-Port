#include <bit>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "formats/PlayerDataWad.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

constexpr usize kRecordSize = 0x180;

void putU32(std::vector<u8>& bytes, usize offset, u32 value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(&bytes[offset], &value, 4);
}

void putF32(std::vector<u8>& bytes, usize offset, f32 value) {
    putU32(bytes, offset, std::bit_cast<u32>(value));
}

/** A wad with one PDAT record after the header and its directory at the end. */
std::vector<u8> sampleWad() {
    const usize record = 16;
    const usize directory = record + kRecordSize;
    std::vector<u8> bytes(directory + 16, 0);
    putU32(bytes, 0, static_cast<u32>(directory));
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
    bytes[directory] = 'T';
    bytes[directory + 1] = 'A';
    bytes[directory + 2] = 'D';
    bytes[directory + 3] = 'P';
    putU32(bytes, directory + 4, static_cast<u32>(record));
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
}

TEST_CASE("damaged class wads are rejected", "[formats][pdata]") {
    std::vector<u8> bytes = sampleWad();
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

} // namespace
