#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

namespace {

using namespace gdl;

TEST_CASE("ByteWriter appends little-endian integers after what the vector holds", "[io][bytes]") {
    std::vector<u8> bytes{0xAA};
    ByteWriter writer(bytes);
    writer.writeU8(0x01);
    writer.writeU16(0x0302);
    writer.writeU32(0x07060504U);
    writer.writeU64(0x0F0E0D0C0B0A0908ULL);
    writer.writeS32(-2);
    const std::vector<u8> expected{0xAA, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                   0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xFE, 0xFF, 0xFF, 0xFF};
    CHECK(bytes == expected);
}

TEST_CASE("ByteWriter puts a float's bits, raw bytes and bare characters", "[io][bytes]") {
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.writeF32(1.0f);
    writer.writeF32(-0.0f);
    writer.writeBytes(std::array<u8, 2>{0x10, 0x20});
    writer.writeString("GDL");
    writer.writeString("");
    const std::vector<u8> expected{0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00,
                                   0x80, 0x10, 0x20, 'G',  'D',  'L'};
    CHECK(bytes == expected);
}

TEST_CASE("ByteReader reads back what ByteWriter wrote", "[io][bytes]") {
    constexpr u64 kWide = std::numeric_limits<u64>::max() - 1;
    constexpr s32 kLowest = std::numeric_limits<s32>::min();
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.writeU64(kWide);
    writer.writeF32(-2.5f);
    writer.writeS32(kLowest);
    writer.writeU16(0xBEEF);
    writer.writeString("name");
    ByteReader reader(bytes);
    CHECK(reader.readU64() == kWide);
    CHECK(reader.readF32() == -2.5f);
    CHECK(reader.readS32() == kLowest);
    CHECK(reader.readU16() == 0xBEEF);
    CHECK(reader.readString(4) == "name");
    CHECK(reader.atEnd());
}

} // namespace
