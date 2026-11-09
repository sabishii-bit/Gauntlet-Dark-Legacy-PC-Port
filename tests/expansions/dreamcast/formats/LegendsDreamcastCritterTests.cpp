#include <array>
#include <bit>
#include <filesystem>
#include <limits>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"
#include "game/critters/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad"))
        SKIP("Dreamcast boss data not installed");
    return root;
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i)
        bytes.at(at + i) = static_cast<u8>(value >> (static_cast<u32>(i) * 8U));
}
usize section(std::span<const u8> bytes, u32 tag) {
    const usize directory = readU32LE(bytes, 0);
    for (usize i = 0; i < readU32LE(bytes, 4); ++i) {
        if (readU32LE(bytes, directory + i * 12) == tag)
            return readU32LE(bytes, directory + i * 12 + 4);
    }
    FAIL("missing native section");
    return 0;
}

TEST_CASE("Dreamcast critter decoder rejects incomplete directories", "[dreamcast][dc-critter]") {
    CHECK_THROWS_AS(decodeLegendsDreamcastCritter({}), FormatError);
    const std::array<u8, 8> empty{};
    CHECK_THROWS_AS(decodeLegendsDreamcastCritter(empty), FormatError);
}

TEST_CASE("Dreamcast six shipped bosses preserve their own combat records",
          "[dreamcast][dc-critter][assets]") {
    const auto root = nativeRoot();
    struct Expected {
        const char* bank;
        const char* tree;
        usize moves;
        usize types;
        f32 health;
    };
    const std::array rows{
        Expected{"dragon", "DRAGON", 32, 1, 2000}, Expected{"chimera", "CHIM", 58, 4, 3500},
        Expected{"djinn", "DJINN", 25, 1, 3500},   Expected{"drider", "DRIDER", 38, 1, 2000},
        Expected{"skorne1", "BOSS", 25, 1, 2500},  Expected{"skorne2", "BOSS", 19, 1, 3000}};
    for (const auto& row : rows) {
        CAPTURE(row.bank);
        const auto bytes = readFile(root / "critters" / (std::string(row.bank) + ".wad"));
        const auto source = decodeLegendsDreamcastCritter(bytes);
        CHECK(source.moves.size() == row.moves);
        CHECK(source.types.size() == row.types);
        CHECK(source.types.front().maxHealth == row.health);
        CHECK(source.types.front().vertDrift == 0);
        for (usize i = 0; i < source.types.size(); ++i) {
            const auto data = game::CritterData::fromDecoded(row.bank, source, i);
            CHECK(data.kind() == game::CombatantKind::Boss);
            if (i == 0)
                CHECK(data.tree() == row.tree);
            for (const auto& move : data.moves())
                CHECK_FALSE(move.anim.empty());
        }
        auto malformed = bytes;
        put32(malformed, section(bytes, 0x54595045) + 0xcc,
              std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
        CHECK_THROWS_AS(decodeLegendsDreamcastCritter(malformed), FormatError);
        malformed = bytes;
        put32(malformed, readU32LE(bytes, 0) + 8, 4097);
        CHECK_THROWS_AS(decodeLegendsDreamcastCritter(malformed), FormatError);
        malformed = bytes;
        put32(malformed, section(bytes, 0x53465858) + 4, 0); // effect links itself
        CHECK_THROWS_AS(decodeLegendsDreamcastCritter(malformed), FormatError);
        malformed = bytes;
        put32(malformed, section(bytes, 0x53465858) + 4, 0xfffffffeU);
        CHECK_THROWS_AS(decodeLegendsDreamcastCritter(malformed), FormatError);
        CHECK_THROWS_AS(decodeLegendsDreamcastCritter(std::span{bytes}.first(bytes.size() - 1)),
                        FormatError);
    }
}

TEST_CASE("Dreamcast Skorne keeps authored entrance links and morph lifetimes",
          "[dreamcast][dc-critter][assets]") {
    const auto source =
        decodeLegendsDreamcastCritter(readFile(nativeRoot() / "critters/skorne1.wad"));
    REQUIRE(source.moves.size() == 25);
    CHECK(source.moves[2].name == "START");
    CHECK(source.moves[2].link == 3);
    // Both entrance MOVE names are START. The linked row selects a different
    // animation; native MOVE+8 is the name and MOVE+18 is the sequence name.
    CHECK(source.moves[3].name == "START");
    CHECK(source.moves[3].anim == "START2");
    CHECK(source.moves[3].type == game::MoveDefinition::kStart);
    CHECK(source.moves[1].type == game::MoveDefinition::kReady);
    CHECK(source.moves[1].hold == 0);
    CHECK(source.moves[4].type == game::MoveDefinition::kDeath);
    CHECK(source.moves[4].hold == 2);
    CHECK(source.nodes[2].flags == 0); // -300 at+12 is sort bias, not collision flags.
    CHECK(source.nodes[2].damageScale == 1);
    CHECK(source.nodes[2].healthScale == 1);
    for (const auto& damage : source.damages)
        CHECK(damage.morphLife == 15);
    CHECK_FALSE(source.sounds.front().levelFormat.empty());
}

TEST_CASE("Dreamcast target centres and turn limits follow native TYPE consumers",
          "[dreamcast][dc-critter][dc-boss-ai][assets]") {
    const auto root = nativeRoot();
    struct Expected {
        const char* bank;
        f32 centreY;
        f32 turn;
    };
    constexpr f32 pi = std::numbers::pi_v<f32>;
    const std::array rows{Expected{"dragon", -14, 0},
                          Expected{"chimera", 8, pi / 4},
                          Expected{"djinn", 8, 2 * pi},
                          Expected{"drider", 9, pi},
                          Expected{"skorne1", 28, 40 * pi / 180},
                          Expected{"skorne2", 28, 40 * pi / 180}};
    for (const auto& row : rows) {
        CAPTURE(row.bank);
        const auto bytes = readFile(root / "critters" / (std::string(row.bank) + ".wad"));
        const auto source = decodeLegendsDreamcastCritter(bytes);
        for (const auto& type : source.types) {
            CHECK(type.originOffset[0] == 0);
            CHECK(type.originOffset[1] == row.centreY);
            CHECK(type.originOffset[2] == 0);
            CHECK(type.turnLimit == Catch::Approx(row.turn));
        }
        // Mutating the unrelated fields must not shift attack eligibility.
        auto otherFields = bytes;
        const usize at = section(bytes, 0x54595045);
        put32(otherFields, at + 0xb4, std::bit_cast<u32>(123.0f));
        put32(otherFields, at + 0xbc, std::bit_cast<u32>(456.0f));
        put32(otherFields, at + 0xc0, std::bit_cast<u32>(789.0f));
        const auto changed = decodeLegendsDreamcastCritter(otherFields);
        CHECK(changed.types.front().originOffset == source.types.front().originOffset);
        CHECK(changed.types.front().turnLimit == source.types.front().turnLimit);
    }
}
} // namespace
