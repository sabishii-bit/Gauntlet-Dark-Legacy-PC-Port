#include <bit>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

#include "formats/CritterWad.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

TEST_CASE("critter damage parser preserves launch policy and the complete effect tail",
          "[formats][boss-projectiles]") {
    constexpr usize kRecord = 16;
    constexpr usize kDirectory = kRecord + 0x50;
    std::vector<u8> bytes(kDirectory + 16);
    const auto put = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<u8>(value >> (byte * 8));
        }
    };
    put(0, kDirectory);
    put(4, 1);
    put(kDirectory, 0x44414D47); // DAMG directory tag
    put(kDirectory + 4, kRecord);
    put(kDirectory + 8, 1);
    put(kDirectory + 12, 1);
    put(kRecord, 0x40090001); // type 1, launch behavior 0x4009
    put(kRecord + 4, 0x12345678);
    put(kRecord + 0x30, std::bit_cast<u32>(35.0f));
    put(kRecord + 0x34, std::bit_cast<u32>(65.0f));
    put(kRecord + 0x38, std::bit_cast<u32>(7.5f));
    put(kRecord + 0x3C, std::bit_cast<u32>(15.0f));
    put(kRecord + 0x40, 0x00080007);
    put(kRecord + 0x44, 0xFFFF0009);
    put(kRecord + 0x48, std::bit_cast<u32>(0.6f));
    const auto file = parseCritterWad(bytes);
    REQUIRE(file.damages.size() == 1);
    const auto& damage = file.damages[0];
    REQUIRE(damage.type == 1);
    REQUIRE(damage.behaviorFlags == 0x4009);
    REQUIRE(damage.flags == 0x12345678);
    REQUIRE(damage.minSpeed == 35);
    REQUIRE(damage.maxSpeed == 65);
    REQUIRE(damage.gravity == 7.5f);
    REQUIRE(damage.morphLife == 15);
    REQUIRE(damage.sfxIndex == 7);
    REQUIRE(damage.sfx == 8);
    REQUIRE(damage.morph == 9);
    REQUIRE(damage.morphEnd == -1);
    REQUIRE(damage.yawSpread == 0.6f);
    put(kDirectory + 4, kDirectory); // declared record no longer fits
    REQUIRE_THROWS_AS(parseCritterWad(bytes), FormatError);
}
TEST_CASE("critter TYPE decodes home radius separately from MOVE speed",
          "[formats][boss-movement]") {
    constexpr usize kRecord = 16;
    constexpr usize kDirectory = kRecord + 0x140;
    std::vector<u8> bytes(kDirectory + 16);
    const auto put = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<u8>(value >> (byte * 8));
        }
    };
    put(0, kDirectory);
    put(4, 1);
    put(kDirectory, 0x54595045); // TYPE
    put(kDirectory + 4, kRecord);
    put(kDirectory + 8, 1);
    put(kDirectory + 12, 1);
    put(kRecord + 0xA4, std::bit_cast<u32>(999.0f));
    put(kRecord + 0xAC, std::bit_cast<u32>(22.0f));
    put(kRecord + 0xCC, std::bit_cast<u32>(0.5f));
    const auto file = parseCritterWad(bytes);
    REQUIRE(file.types.size() == 1);
    REQUIRE(file.types[0].defaultPos[1] == 999);
    REQUIRE(file.types[0].roamRadius == 22);
    REQUIRE(file.types[0].turnLimit == 0.5f);
}

TEST_CASE("critter patterns preserve all eight signed slots and phase gates",
          "[formats][boss-attacks]") {
    constexpr usize kRecord = 16;
    constexpr usize kDirectory = kRecord + 0x50;
    std::vector<u8> bytes(kDirectory + 16);
    const auto put = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<u8>(value >> (byte * 8));
        }
    };
    put(0, kDirectory);
    put(4, 1);
    put(kDirectory, 0x5054524E); // PTRN
    put(kDirectory + 4, kRecord);
    put(kDirectory + 8, 1);
    put(kRecord + 0x10, 0x1002);
    put(kRecord + 0x14, std::bit_cast<u32>(3.0f));
    put(kRecord + 0x20, 0x0025002B);
    put(kRecord + 0x24, 0xFFFFFFFF);
    put(kRecord + 0x28, 0xFFFFFFFF);
    put(kRecord + 0x2C, 0xFFFF0007);
    put(kRecord + 0x40, std::bit_cast<u32>(1.5f));
    put(kRecord + 0x44, std::bit_cast<u32>(2.5f));
    put(kRecord + 0x48, std::bit_cast<u32>(20.0f));
    const auto file = parseCritterWad(bytes);
    REQUIRE(file.patterns.size() == 1);
    const auto& pattern = file.patterns.front();
    REQUIRE(pattern.flags == 0x1002);
    REQUIRE(pattern.cooldown == 3);
    REQUIRE(pattern.moves[0] == 43);
    REQUIRE(pattern.moves[1] == 37);
    REQUIRE(pattern.moves[2] == -1);
    REQUIRE(pattern.moves[6] == 7);
    REQUIRE(pattern.moves[7] == -1);
    REQUIRE(pattern.target.minRateScale == 1.5f);
    REQUIRE(pattern.target.maxRateScale == 2.5f);
    REQUIRE(pattern.target.idleGate == 20);
    put(kDirectory + 8, 2);
    REQUIRE_THROWS_AS(parseCritterWad(bytes), FormatError);
}

} // namespace
