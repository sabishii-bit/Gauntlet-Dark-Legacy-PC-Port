#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"
#include "game/critters/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

/** A section's directory code as the native WAD stores it: its first letter in the high byte. */
constexpr u32 nativeTag(std::string_view code) {
    return (u32{static_cast<u8>(code[0])} << 24U) | (u32{static_cast<u8>(code[1])} << 16U) |
           (u32{static_cast<u8>(code[2])} << 8U) | u32{static_cast<u8>(code[3])};
}

struct SyntheticSection {
    std::string_view tag;
    usize stride;
    u32 count;
    usize room; ///< records' worth of bytes laid out for it
};

// Every section the decoder reads, with its Dreamcast stride.
constexpr std::array kSyntheticSections{
    SyntheticSection{"DESC", 0x24, 1, 1},  SyntheticSection{"MOVE", 0x80, 6, 6},
    SyntheticSection{"PTRN", 0x44, 1, 1},  SyntheticSection{"DAMG", 0x48, 1, 1},
    SyntheticSection{"SFXX", 0x48, 2, 2},  SyntheticSection{"NODE", 0x48, 1, 1},
    SyntheticSection{"TYPE", 0x118, 2, 2}, SyntheticSection{"ADDA", 0x28, 0, 1}};

/** The smallest valid CRITTERS WAD: a boss with one child type, six moves straddling the
 * native 0x85..0x87 attack types, a two-move pattern, a damage, an effect that starts a
 * second, and a hit node. ADDA is empty with room for one record before the directory. */
class SyntheticCritters {
public:
    SyntheticCritters() {
        test::SyntheticBytes out;
        out.putU32(0).putU32(static_cast<u32>(kSyntheticSections.size()));
        for (const auto& section : kSyntheticSections) {
            m_offsets.push_back(out.size());
            out.putZeros(section.stride * section.room);
        }
        m_directory = out.size();
        for (usize i = 0; i < kSyntheticSections.size(); ++i) {
            out.putU32(nativeTag(kSyntheticSections[i].tag))
                .putU32(static_cast<u32>(m_offsets[i]))
                .putU32(kSyntheticSections[i].count);
        }
        bytes = out.bytes();
        test::setU32LE(bytes, 0, static_cast<u32>(m_directory));

        test::setText(bytes, record("DESC"), 16, "dragon");
        test::setText(bytes, record("DESC") + 0x10, 8, "DRAGON");
        setS16(record("DESC") + 0x18, 4);
        constexpr std::array<u32, 6> kMoveTypes{32, 0x84, 0x85, 0x86, 0x87, 17};
        for (usize move = 0; move < kMoveTypes.size(); ++move) {
            test::setU32LE(bytes, record("MOVE", move), kMoveTypes[move]);
            // No damage records, link or effects.
            for (const usize field : std::array<usize, 5>{0x5c, 0x5e, 0x64, 0x74, 0x78}) {
                setS16(record("MOVE", move) + field, -1);
            }
        }
        // The pattern plays moves 1 and 2, then ends.
        constexpr std::array<s16, 8> kPattern{1, 2, -1, -1, -1, -1, -1, -1};
        for (usize slot = 0; slot < kPattern.size(); ++slot) {
            setS16(record("PTRN") + 0x14 + slot * 2, kPattern[slot]);
        }
        for (const usize field : std::array<usize, 4>{0x3c, 0x3e, 0x40, 0x42}) {
            setS16(record("DAMG") + field, -1);
        }
        test::setU32LE(bytes, record("SFXX", 0) + 4, 1);
        test::setU32LE(bytes, record("SFXX", 1) + 4, 0xffffffffU);
        test::setText(bytes, record("NODE"), 16, "HEAD");
        setS16(record("NODE") + 0x46, -1);

        const usize boss = record("TYPE", 0);
        test::setF32LE(bytes, boss + 0x8c, 4);
        test::setF32LE(bytes, boss + 0x90, 3);
        test::setF32LE(bytes, boss + 0xa0, 3);
        test::setF32LE(bytes, boss + 0xa4, 18.5f);
        test::setF32LE(bytes, boss + 0xa8, 7);
        test::setF32LE(bytes, boss + 0xac, -14);
        test::setF32LE(bytes, boss + 0xb0, .75f);
        test::setF32LE(bytes, boss + 0xcc, 2000);
        setType(boss, {-1, -1, 6, 0, 1, 0, 1, 0, 1, -1});
        const usize child = record("TYPE", 1);
        test::setF32LE(bytes, child + 0xcc, 1200);
        setType(child, {-1, -1, 1, 5, 0, 0, 0, 0, -1, 0});
    }

    usize record(std::string_view tag, usize index = 0) const {
        const usize section = at(tag);
        return m_offsets.at(section) + index * kSyntheticSections.at(section).stride;
    }
    /** The section's directory row: its code, then its offset, then its count. */
    usize entry(std::string_view tag) const { return m_directory + at(tag) * 12; }
    void setS16(usize offset, s16 value) {
        test::setU16LE(bytes, offset, std::bit_cast<u16>(value));
    }

    std::vector<u8> bytes;

private:
    /** TYPE +0xDC: the hit sounds, the move, pattern and hit-node slices, child and parent. */
    void setType(usize type, const std::array<s16, 10>& references) {
        constexpr std::array<usize, 10> kFields{0xdc, 0xde, 0xec, 0xee, 0xf0,
                                                0xf2, 0xf4, 0xf6, 0xf8, 0xfa};
        for (usize i = 0; i < kFields.size(); ++i) {
            setS16(type + kFields[i], references[i]);
        }
    }
    static usize at(std::string_view tag) {
        const auto found = std::ranges::find(kSyntheticSections, tag, &SyntheticSection::tag);
        REQUIRE(found != kSyntheticSections.end());
        return static_cast<usize>(found - kSyntheticSections.begin());
    }

    std::vector<usize> m_offsets;
    usize m_directory = 0;
};

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

TEST_CASE("Dreamcast critter decoder reads a synthetic native directory",
          "[dreamcast][dc-critter]") {
    SyntheticCritters wad;
    const auto file = decodeLegendsDreamcastCritter(wad.bytes);
    REQUIRE(file.descriptors.size() == 1);
    CHECK(file.descriptors[0].name == "dragon");
    CHECK(file.descriptors[0].prefix == "DRAGON");
    CHECK(file.descriptors[0].type == 4);
    REQUIRE(file.moves.size() == 6);
    std::vector<s32> types;
    types.reserve(file.moves.size());
    for (const auto& move : file.moves) {
        types.push_back(move.type);
    }
    // 8c0895e0's attack types 0x85..0x87 are GDL's 0x86..0x88: GDL inserted SPRAY at 0x85.
    CHECK(types == std::vector<s32>{game::MoveDefinition::kReady, 0x84, 0x86, 0x87,
                                    game::MoveDefinition::kTargetArea,
                                    game::MoveDefinition::kDeath});
    REQUIRE(file.patterns.size() == 1);
    CHECK(file.patterns[0].moves[0] == 1);
    CHECK(file.patterns[0].moves[1] == 2);
    CHECK(file.patterns[0].moves[2] == -1);
    CHECK(file.damages.size() == 1);
    REQUIRE(file.sounds.size() == 2);
    CHECK(file.sounds[0].link == 1);
    CHECK(file.sounds[1].link == -1);
    REQUIRE(file.nodes.size() == 1);
    CHECK(file.nodes[0].nodeName == "HEAD");
    CHECK(file.attachments.empty());
    REQUIRE(file.types.size() == 2);
    const auto& boss = file.types[0];
    CHECK(boss.roamRadius == 3);
    CHECK(boss.floorOffset == 18.5f);
    // TYPE +0xA8 is the camera's attention height; the target centre is +0xAC alone.
    CHECK(boss.originOffset == std::array<f32, 3>{0, -14, 0});
    CHECK(boss.turnLimit == .75f);
    CHECK(boss.maxHealth == 2000);
    CHECK(boss.childIndex == 1);
    CHECK(file.types[1].parentIndex == 0);
    CHECK(file.types[1].moveIndex == 5);

    auto camera = wad.bytes;
    test::setF32LE(camera, wad.record("TYPE") + 0xa8, 99);
    const auto elsewhere = decodeLegendsDreamcastCritter(camera);
    CHECK(elsewhere.types[0].originOffset == boss.originOffset);
    CHECK(elsewhere.types[0].floorOffset == boss.floorOffset);
    CHECK(elsewhere.types[0].turnLimit == boss.turnLimit);

    SECTION("an empty section may share another's offset") {
        test::setU32LE(wad.bytes, wad.entry("ADDA") + 4, static_cast<u32>(wad.record("MOVE")));
        CHECK(decodeLegendsDreamcastCritter(wad.bytes).moves.size() == 6);
    }
    SECTION("a pattern ends at its first negative move") {
        wad.setS16(wad.record("PTRN") + 0x16, -1);
        wad.setS16(wad.record("PTRN") + 0x18, 99);
        CHECK(decodeLegendsDreamcastCritter(wad.bytes).patterns[0].moves[2] == 99);
    }
}

TEST_CASE("Dreamcast critter decoder names the rule a malformed directory breaks",
          "[dreamcast][dc-critter]") {
    const SyntheticCritters valid;
    using Edit = void (*)(const SyntheticCritters&, std::vector<u8>&);
    struct Mutation {
        const char* what;
        Edit edit;
        const char* rule;
    };
    // Negative native shorts are written as their bits: 0xffff is -1.
    const std::array mutations{
        Mutation{"directory inside the header",
                 [](const auto&, auto& bytes) { test::setU32LE(bytes, 0, 4); },
                 "invalid directory"},
        Mutation{"more sections than a directory holds",
                 [](const auto&, auto& bytes) { test::setU32LE(bytes, 4, 65); },
                 "invalid directory"},
        Mutation{"directory past the end of the file",
                 [](const auto&, auto& bytes) { bytes.pop_back(); }, "truncated record"},
        Mutation{"two rows for one section",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.entry("ADDA"), nativeTag("DESC"));
                 },
                 "duplicate section"},
        Mutation{"a section left out",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.entry("ADDA"), nativeTag("ADDX"));
                 },
                 "missing section"},
        Mutation{
            "records inside the header",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.entry("DESC") + 4, 4); },
            "overlapping or truncated section"},
        Mutation{
            "records running into the next section",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.entry("MOVE") + 8, 7); },
            "overlapping or truncated section"},
        Mutation{
            "records running into the directory",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.entry("ADDA") + 8, 2); },
            "overlapping or truncated section"},
        Mutation{"a section at the record budget that does not fit",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.entry("MOVE") + 8, 4096);
                 },
                 "overlapping or truncated section"},
        Mutation{"a section over the record budget",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.entry("MOVE") + 8, 4097);
                 },
                 "record budget exceeded"},
        Mutation{"two nonempty sections on the same records",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.entry("PTRN") + 4,
                                    static_cast<u32>(wad.record("MOVE")));
                 },
                 "aliased nonempty sections"},
        Mutation{
            "an additional-tree record",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.entry("ADDA") + 8, 1); },
            "additional-tree records are not supported"},
        Mutation{
            "no type",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.entry("TYPE") + 8, 0); },
            "empty combat definition"},
        Mutation{"a nonfinite health",
                 [](const auto& wad, auto& bytes) {
                     test::setF32LE(bytes, wad.record("TYPE") + 0xcc,
                                    std::numeric_limits<f32>::quiet_NaN());
                 },
                 "non-finite value"},
        Mutation{"no health",
                 [](const auto& wad, auto& bytes) {
                     test::setF32LE(bytes, wad.record("TYPE") + 0xcc, 0);
                 },
                 "invalid body dimensions or health"},
        Mutation{"a control character in a name",
                 [](const auto& wad, auto& bytes) { bytes.at(wad.record("DESC") + 2) = 7; },
                 "invalid name"},
        Mutation{"a type without a descriptor",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE"), 0xffffU);
                 },
                 "missing descriptor"},
        Mutation{"a descriptor past the table",
                 [](const auto& wad, auto& bytes) { test::setU16LE(bytes, wad.record("TYPE"), 1); },
                 "invalid record reference"},
        Mutation{"a move slice past the moves",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE", 1) + 0xee, 6);
                 },
                 "invalid type range"},
        Mutation{"a pattern slice past the patterns",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE") + 0xf2, 1);
                 },
                 "invalid type range"},
        Mutation{"a hit-node slice past the nodes",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE") + 0xf4, 2);
                 },
                 "invalid type range"},
        Mutation{"a negative slice",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE") + 0xf0, 0xffffU);
                 },
                 "invalid type range"},
        Mutation{"a move linking outside its own type",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("MOVE", 5) + 0x64, 1);
                 },
                 "invalid record reference"},
        Mutation{"a pattern naming a move outside its type",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("PTRN") + 0x16, 6);
                 },
                 "invalid record reference"},
        Mutation{"a move naming a missing damage record",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("MOVE") + 0x5c, 1);
                 },
                 "invalid record reference"},
        Mutation{"a hit node naming a missing damage record",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("NODE") + 0x46, 1);
                 },
                 "invalid record reference"},
        Mutation{"a damage naming a missing effect",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("DAMG") + 0x40, 2);
                 },
                 "invalid record reference"},
        Mutation{"a type naming a missing hit effect",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE") + 0xde, 2);
                 },
                 "invalid record reference"},
        Mutation{"an effect linking past the table",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.record("SFXX", 1) + 4, 2);
                 },
                 "invalid record reference"},
        Mutation{"an effect link under the end marker",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.record("SFXX") + 4, 0xfffffffeU);
                 },
                 "invalid record reference"},
        Mutation{"an effect chain that loops",
                 [](const auto& wad, auto& bytes) {
                     test::setU32LE(bytes, wad.record("SFXX", 1) + 4, 0);
                 },
                 "cyclic effect chain"},
        Mutation{
            "an effect starting itself",
            [](const auto& wad, auto& bytes) { test::setU32LE(bytes, wad.record("SFXX") + 4, 0); },
            "cyclic effect chain"},
        Mutation{"a child chain that loops",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE", 1) + 0xf8, 0);
                 },
                 "cyclic child chain"},
        Mutation{"a type that is its own child",
                 [](const auto& wad, auto& bytes) {
                     test::setU16LE(bytes, wad.record("TYPE") + 0xf8, 0);
                 },
                 "cyclic child chain"},
    };
    for (const auto& mutation : mutations) {
        CAPTURE(mutation.what);
        auto bytes = valid.bytes;
        mutation.edit(valid, bytes);
        CHECK_THROWS_WITH(decodeLegendsDreamcastCritter(bytes),
                          std::string{"Dreamcast critter: "} + mutation.rule);
    }
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
