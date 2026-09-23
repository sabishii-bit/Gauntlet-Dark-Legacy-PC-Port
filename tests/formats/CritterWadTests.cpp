#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "formats/CritterWad.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

TEST_CASE("critter damage parser preserves launch policy and the complete effect tail",
          "[formats][boss-projectiles]") {
    constexpr std::size_t kRecord = 16;
    constexpr std::size_t kDirectory = kRecord + 0x50;
    std::vector<std::uint8_t> bytes(kDirectory + 16);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
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
    put(kRecord + 0x30, std::bit_cast<std::uint32_t>(35.0f));
    put(kRecord + 0x34, std::bit_cast<std::uint32_t>(65.0f));
    put(kRecord + 0x38, std::bit_cast<std::uint32_t>(7.5f));
    put(kRecord + 0x3C, std::bit_cast<std::uint32_t>(15.0f));
    put(kRecord + 0x40, 0x00080007);
    put(kRecord + 0x44, 0xFFFF0009);
    put(kRecord + 0x48, std::bit_cast<std::uint32_t>(0.6f));
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
    constexpr std::size_t kRecord = 16;
    constexpr std::size_t kDirectory = kRecord + 0x140;
    std::vector<std::uint8_t> bytes(kDirectory + 16);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
    };
    put(0, kDirectory);
    put(4, 1);
    put(kDirectory, 0x54595045); // TYPE
    put(kDirectory + 4, kRecord);
    put(kDirectory + 8, 1);
    put(kDirectory + 12, 1);
    put(kRecord + 0xA4, std::bit_cast<std::uint32_t>(999.0f));
    put(kRecord + 0xAC, std::bit_cast<std::uint32_t>(22.0f));
    put(kRecord + 0xCC, std::bit_cast<std::uint32_t>(0.5f));
    const auto file = parseCritterWad(bytes);
    REQUIRE(file.types.size() == 1);
    REQUIRE(file.types[0].defaultPos[1] == 999);
    REQUIRE(file.types[0].roamRadius == 22);
    REQUIRE(file.types[0].turnLimit == 0.5f);
}

} // namespace
