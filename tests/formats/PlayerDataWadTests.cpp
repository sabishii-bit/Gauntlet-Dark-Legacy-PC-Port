#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

#include "SyntheticBytes.h"
#include "formats/PlayerDataWad.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::setF32LE;
using test::setText;
using test::setU16LE;
using test::setU32LE;

constexpr usize kRecordSize = 0x180;
/** The fixed width of an effect's tree and sound names. */
constexpr usize kNameField = 16;

/** A wad with one PDAT record after the header and its directory at the end. */
std::vector<u8> sampleWad() {
    const usize record = 16;
    const usize directory = record + kRecordSize;
    std::vector<u8> bytes(directory + 16, 0);
    setU32LE(bytes, 0, static_cast<u32>(directory));
    setU32LE(bytes, 4, 1);
    bytes[record] = 15; // effect count
    bytes[record + 2] = 14;
    setF32LE(bytes, record + 0x28, 600.0f);
    setF32LE(bytes, record + 0x2C, 999.0f);
    setF32LE(bytes, record + 0x30, 350.0f);
    setF32LE(bytes, record + 0x34, 750.0f);
    setF32LE(bytes, record + 0x38, 300.0f);
    setF32LE(bytes, record + 0x3C, 700.0f);
    setF32LE(bytes, record + 0x40, 100.0f);
    setF32LE(bytes, record + 0x44, 500.0f);
    setF32LE(bytes, record + 0x48, 5.0f);
    setF32LE(bytes, record + 0x4C, 1.5f);
    setF32LE(bytes, record + 0x50, 4.4f);
    setF32LE(bytes, record + 0x54, 2.5f);
    setF32LE(bytes, record + 0x58, 1.25f);
    setF32LE(bytes, record + 0x5C, -0.5f);
    setF32LE(bytes, record + 0x60, 0.5f);
    setF32LE(bytes, record + 0x64, 1.5f);
    setF32LE(bytes, record + 0x68, 0.0f); // the first tier's glow offset and scale
    setF32LE(bytes, record + 0x6C, -0.35f);
    setF32LE(bytes, record + 0x70, 0.75f);
    setF32LE(bytes, record + 0xE0, 0.45f);
    setF32LE(bytes, record + 0xE4, 0.55f);
    setF32LE(bytes, record + 0xE8, 0.55f);
    constexpr usize kTenthTier = usize{9} * 12;
    setF32LE(bytes, record + 0x68 + kTenthTier + 4, -0.5f); // the tenth tier's offset, y
    setF32LE(bytes, record + 0xE0 + kTenthTier + 8, 0.9f);  // and scale, z
    setF32LE(bytes, record + 0x164, -1.0f);
    setF32LE(bytes, record + 0x168, 5.0f);
    setF32LE(bytes, record + 0x16C, 0.75f);
    setF32LE(bytes, record + 0x170, 1.25f);
    setF32LE(bytes, record + 0x174, 6.0f);
    setF32LE(bytes, record + 0x178, -0.5f);
    setF32LE(bytes, record + 0x17C, 2.5f);
    bytes[directory] = 'T';
    bytes[directory + 1] = 'A';
    bytes[directory + 2] = 'D';
    bytes[directory + 3] = 'P';
    setU32LE(bytes, directory + 4, static_cast<u32>(record));
    setU32LE(bytes, directory + 8, 1);
    setU32LE(bytes, directory + 12, 1);
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
    REQUIRE(record.streakForward == 2.5f);
    REQUIRE(record.weaponOffset == std::array<f32, 3>{-0.5f, 0.5f, 1.5f});
    REQUIRE(record.familiarOffset == std::array<f32, 3>{-1.0f, 5.0f, 0.75f});
    REQUIRE(record.familiarShotOffset == std::array<f32, 3>{1.25f, 6.0f, -0.5f});
    REQUIRE(record.weaponGlowOffsets[0] == std::array<f32, 3>{0.0f, -0.35f, 0.75f});
    REQUIRE(record.weaponGlowScales[0] == std::array<f32, 3>{0.45f, 0.55f, 0.55f});
    REQUIRE(record.weaponGlowOffsets[9] == std::array<f32, 3>{0.0f, -0.5f, 0.0f});
    REQUIRE(record.weaponGlowScales[9] == std::array<f32, 3>{0.0f, 0.0f, 0.9f});
    REQUIRE(record.weaponGlowOffsets[4] == std::array<f32, 3>{0.0f, 0.0f, 0.0f});
}

TEST_CASE("damaged class wads are rejected", "[formats][pdata]") {
    std::vector<u8> bytes = sampleWad();
    bytes.resize(8);
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    bytes[bytes.size() - 16] = 'X'; // not a PDAT section
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    setU32LE(bytes, bytes.size() - 12, 0x1000); // record beyond the file
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);

    bytes = sampleWad();
    setU32LE(bytes, 4, 100); // more directory entries than fit
    REQUIRE_THROWS_AS(parsePlayerDataWad(bytes), FormatError);
}

TEST_CASE("a class wad gives its moves: the effects they show and the strikes they make",
          "[formats][pdata]") {
    // One effect, two strikes, then the class record, then the directory of all three.
    constexpr usize kEffect = 0x50;
    constexpr usize kStrike = 0x58;
    const usize effects = 16;
    const usize strikes = effects + kEffect;
    const usize record = strikes + 2 * kStrike;
    const usize directory = record + kRecordSize;
    constexpr usize kEntry = 16;
    std::vector<u8> bytes(directory + 3 * kEntry, 0);
    setU32LE(bytes, 0, static_cast<u32>(directory));
    setU32LE(bytes, 4, 3);
    const auto section = [&](usize slot, std::string_view tag, usize offset, u32 count) {
        const usize at = directory + slot * kEntry;
        for (usize c = 0; c < 4; ++c) {
            bytes[at + c] = static_cast<u8>(tag[3 - c]); // tags lie the other way round
        }
        setU32LE(bytes, at + 4, static_cast<u32>(offset));
        setU32LE(bytes, at + 8, count);
        setU32LE(bytes, at + 12, count);
    };
    section(0, "SFXX", effects, 1);
    section(1, "DAMG", strikes, 2);
    section(2, "PDAT", record, 1);
    setU16LE(bytes, record, 1);     // effects
    setU16LE(bytes, record + 2, 2); // strikes
    for (usize move = 0; move < PlayerClassRecord::kMoveCount; ++move) {
        setU16LE(bytes, record + 0x0C + move * 2, static_cast<u16>(-1));
    }
    setU16LE(bytes, record + 0x0C + usize{5} * 2, 0); // turbo B starts at the first strike
    setU32LE(bytes, effects, 4);
    setU32LE(bytes, effects + 4, 0xFFFFFFFF);
    setText(bytes, effects + 0x10, kNameField, "WAR_POWERB");
    setText(bytes, effects + 0x20, kNameField, "S_WARTURBOB");
    setF32LE(bytes, effects + 0x38, 5.0f);
    setF32LE(bytes, effects + 0x48, 2.0f);
    setF32LE(bytes, effects + 0x40, 0.5f);
    setF32LE(bytes, effects + 0x44, 3.0f);
    setU16LE(bytes, effects + 0x32, static_cast<u16>(-150));
    setU32LE(bytes, effects + 0x4C, 0x7F12AB34);
    setU16LE(bytes, strikes, 4);
    setF32LE(bytes, strikes + 0x0C, 12.0f);
    setF32LE(bytes, strikes + 0x14, 0.5f);
    setF32LE(bytes, strikes + 0x24, -1.0f);
    setF32LE(bytes, strikes + 0x38, 50.0f);
    setU16LE(bytes, strikes + 0x48, 0);
    setU16LE(bytes, strikes + 0x4E, 1);
    setU16LE(bytes, strikes + kStrike, 2);
    setF32LE(bytes, strikes + kStrike + 0x08, 10.0f);
    setF32LE(bytes, strikes + kStrike + 0x1C, 6.0f);
    setF32LE(bytes, strikes + kStrike + 0x34, 5.0f);
    setF32LE(bytes, strikes + kStrike + 0x38, -1.5f);
    setF32LE(bytes, strikes + kStrike + 0x3C, 30.0f);
    setF32LE(bytes, strikes + kStrike + 0x40, 40.0f);
    setU16LE(bytes, strikes + kStrike + 0x4E, static_cast<u16>(-1));
    setU16LE(bytes, strikes + kStrike + 0x50, 9);

    const PlayerClassRecord parsed = parsePlayerDataWad(bytes);
    REQUIRE(PlayerClassRecord::kMoveNames[5] == "turboB");
    REQUIRE(parsed.moves[5] == 0);
    REQUIRE(parsed.moves[6] == -1);
    REQUIRE(parsed.effects.size() == 1);
    REQUIRE(parsed.effects[0].tree == "WAR_POWERB");
    REQUIRE(parsed.effects[0].sound == "S_WARTURBOB");
    REQUIRE(parsed.effects[0].next == -1);
    REQUIRE(parsed.effects[0].offset == std::array<f32, 3>{0.0f, 5.0f, 0.0f});
    REQUIRE(parsed.effects[0].scale == 2.0f);
    CHECK(parsed.effects[0].lifetime == 0.5f);
    CHECK(parsed.effects[0].radius == 3.0f);
    CHECK(parsed.effects[0].alphaMod == -150);
    CHECK(parsed.effects[0].color == 0x7F12AB34);
    REQUIRE(parsed.strikes.size() == 2);
    REQUIRE(parsed.strikes[0].type == 4);
    REQUIRE(parsed.strikes[0].radius == 12.0f);
    REQUIRE(parsed.strikes[0].delay == 0.5f);
    REQUIRE(parsed.strikes[0].amount == 50.0f);
    REQUIRE(parsed.strikes[0].next == 1);
    REQUIRE(parsed.strikes[1].type == 2);
    REQUIRE(parsed.strikes[1].hitRadius == 10.0f);
    REQUIRE(parsed.strikes[1].maxTime == 6.0f);
    REQUIRE(parsed.strikes[1].offset == std::array<f32, 3>{0.0f, 0.0f, 5.0f});
    REQUIRE(parsed.strikes[1].amount == -1.5f);
    REQUIRE(parsed.strikes[1].speedMax == 40.0f);
    REQUIRE(parsed.strikes[1].startFrame == 9);
    // A wad with no such sections has no moves to give.
    REQUIRE(parsePlayerDataWad(sampleWad()).strikes.empty());
}

} // namespace
