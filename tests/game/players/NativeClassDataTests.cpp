#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/players/ClassData.h"
#include "game/players/NativeClassData.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr usize kHeaderSize = 16;
constexpr usize kEffectSize = 0x50;
constexpr usize kStrikeSize = 0x58;
constexpr usize kClassSize = 0x180;
constexpr usize kEffectOffset = kHeaderSize;
constexpr usize kStrikeOffset = kEffectOffset + kEffectSize;
constexpr usize kClassOffset = kStrikeOffset + kStrikeSize;
constexpr usize kDirectoryOffset = kClassOffset + kClassSize;

void padTo(test::ByteWriter& writer, usize offset) {
    REQUIRE(writer.size() <= offset);
    writer.putZeros(offset - writer.size());
}

void putFloat(test::ByteWriter& writer, f32 value) {
    writer.putU32(std::bit_cast<u32>(value));
}

std::vector<u8> nativeClassWad() {
    test::ByteWriter writer;
    writer.putU32(static_cast<u32>(kDirectoryOffset)).putU32(3).putZeros(8);
    // One linked particle effect, with a node name in the sound field.
    writer.putU32(MoveEffect::kParticleFlags).putS32(-1);
    padTo(writer, kEffectOffset + 0x10);
    writer.putText("HAND_GLOW");
    padTo(writer, kEffectOffset + 0x20);
    writer.putText("R_WRIST");
    padTo(writer, kEffectOffset + 0x32);
    writer.putU16(150);
    padTo(writer, kEffectOffset + 0x34);
    for (const f32 value : {1.0f, 2.0f, 3.0f}) {
        putFloat(writer, value);
    }
    putFloat(writer, 0.5f);
    putFloat(writer, 3.0f);
    putFloat(writer, 1.25f);
    writer.putU32(0x7F12AB34);
    padTo(writer, kStrikeOffset);
    writer.putU16(2).putU16(0x2010).putU32(0x100021);
    putFloat(writer, 2.0f); // hit radius
    putFloat(writer, 3.0f); // radius
    writer.putU32(0);
    for (const f32 value : {0.5f, 1.0f, 6.0f, -0.5f, 0.75f}) {
        putFloat(writer, value); // delay, min/max time, angle, arc
    }
    writer.putU32(0);
    for (const f32 value : {4.0f, 5.0f, 6.0f, -2.0f, 30.0f, 40.0f, 0.5f}) {
        putFloat(writer, value); // offset, amount, min/max speed, weight
    }
    writer.putU16(0).putU16(0).putU16(0).putU16(0xFFFF);
    writer.putU16(9).putU16(30).putU16(57);
    padTo(writer, kClassOffset);
    writer.putU16(1).putU16(1).putZeros(8);
    for (usize move = 0; move < 12; ++move) {
        writer.putU16(move == 9 ? 0xFFFF : 0);
    }
    padTo(writer, kClassOffset + 0x28);
    for (const f32 value : {600.0f, 999.0f, 350.0f, 750.0f, 300.0f, 700.0f, 100.0f, 500.0f, 5.0f,
                            1.5f, 4.4f, 2.5f, 1.25f, -0.5f, 0.5f, 1.5f}) {
        putFloat(writer, value); // ranges, body, powerup duration, weapon offset
    }
    for (usize component = 0; component < ClassStats::kGlowTiers * 3 * 2; ++component) {
        putFloat(writer, static_cast<f32>(component) * 0.25f);
    }
    padTo(writer, kClassOffset + 0x164);
    for (const f32 value : {-1.0f, 5.0f, 0.75f, 1.25f, 6.0f, -0.5f, 2.5f}) {
        putFloat(writer, value); // familiar offsets, streak head lead
    }
    REQUIRE(writer.size() == kDirectoryOffset);
    writer.putText("XXFS").putU32(static_cast<u32>(kEffectOffset)).putU32(1).putU32(1);
    writer.putText("GMAD").putU32(static_cast<u32>(kStrikeOffset)).putU32(1).putU32(1);
    writer.putText("TADP").putU32(static_cast<u32>(kClassOffset)).putU32(1).putU32(1);
    return writer.bytes();
}

void sameFloat(f32 actual, f32 expected) {
    CHECK(actual == Catch::Approx(expected).margin(0.00001));
}

void sameVector(Vec3 actual, Vec3 expected) {
    sameFloat(actual.x, expected.x);
    sameFloat(actual.y, expected.y);
    sameFloat(actual.z, expected.z);
}

void sameClass(const ClassStats& actual, const ClassStats& expected) {
    sameFloat(actual.fightMin, expected.fightMin);
    sameFloat(actual.fightMax, expected.fightMax);
    sameFloat(actual.speedMin, expected.speedMin);
    sameFloat(actual.speedMax, expected.speedMax);
    sameFloat(actual.armorMin, expected.armorMin);
    sameFloat(actual.armorMax, expected.armorMax);
    sameFloat(actual.magicMin, expected.magicMin);
    sameFloat(actual.magicMax, expected.magicMax);
    sameFloat(actual.height, expected.height);
    sameFloat(actual.width, expected.width);
    sameFloat(actual.attentionY, expected.attentionY);
    sameFloat(actual.collisionY, expected.collisionY);
    sameFloat(actual.powerupTime, expected.powerupTime);
    sameFloat(actual.streakForward, expected.streakForward);
    sameVector(actual.weaponOffset, expected.weaponOffset);
    sameVector(actual.familiarOffset, expected.familiarOffset);
    sameVector(actual.familiarShotOffset, expected.familiarShotOffset);
    for (usize tier = 0; tier < ClassStats::kGlowTiers; ++tier) {
        sameVector(actual.weaponGlowOffsets[tier], expected.weaponGlowOffsets[tier]);
        sameVector(actual.weaponGlowScales[tier], expected.weaponGlowScales[tier]);
    }
    CHECK(actual.moves.turboAClose == expected.moves.turboAClose);
    CHECK(actual.moves.turboALow == expected.moves.turboALow);
    CHECK(actual.moves.turboAStep == expected.moves.turboAStep);
    CHECK(actual.moves.turboA360 == expected.moves.turboA360);
    CHECK(actual.moves.turboAThrow == expected.moves.turboAThrow);
    CHECK(actual.moves.turboB == expected.moves.turboB);
    CHECK(actual.moves.turboC1 == expected.moves.turboC1);
    CHECK(actual.moves.turboC2 == expected.moves.turboC2);
    CHECK(actual.moves.combo1 == expected.moves.combo1);
    CHECK(actual.moves.comboHit == expected.moves.comboHit);
    REQUIRE(actual.moveEffects.size() == expected.moveEffects.size());
    for (usize index = 0; index < actual.moveEffects.size(); ++index) {
        CAPTURE(index);
        const auto& a = actual.moveEffects[index];
        const auto& b = expected.moveEffects[index];
        CHECK(a.next == b.next);
        CHECK(a.tree == b.tree);
        CHECK(a.sound == b.sound);
        CHECK(a.flags == b.flags);
        sameVector(a.offset, b.offset);
        sameFloat(a.scale, b.scale);
        sameFloat(a.lifetime, b.lifetime);
        sameFloat(a.radius, b.radius);
        CHECK(a.alphaMod == b.alphaMod);
        CHECK(a.color == b.color);
    }
    REQUIRE(actual.moveStrikes.size() == expected.moveStrikes.size());
    for (usize index = 0; index < actual.moveStrikes.size(); ++index) {
        CAPTURE(index);
        const auto& a = actual.moveStrikes[index];
        const auto& b = expected.moveStrikes[index];
        CHECK(a.type == b.type);
        sameFloat(a.hitRadius, b.hitRadius);
        sameFloat(a.radius, b.radius);
        sameFloat(a.delay, b.delay);
        sameFloat(a.maxTime, b.maxTime);
        sameFloat(a.arc, b.arc);
        sameVector(a.offset, b.offset);
        sameFloat(a.amount, b.amount);
        sameFloat(a.speed, b.speed);
        sameFloat(a.angle, b.angle);
        CHECK(a.damageType == b.damageType);
        CHECK(a.effect == b.effect);
        CHECK(a.hitEffect == b.hitEffect);
        CHECK(a.loopEffect == b.loopEffect);
        CHECK(a.next == b.next);
        CHECK(a.startFrame == b.startFrame);
        CHECK(a.endFrame == b.endFrame);
        CHECK(a.flags == b.flags);
        CHECK(a.help == b.help);
    }
}

TEST_CASE("native class tuning preserves combat and presentation tables",
          "[game][players][native-class]") {
    const auto stats = parseNativeClassStats(nativeClassWad());
    CHECK(stats.fightMin == 600.0f);
    CHECK(stats.fightMax == 999.0f);
    CHECK(stats.magicMax == 500.0f);
    CHECK(stats.collisionY == 2.5f);
    CHECK(stats.attentionY == 4.4f);
    CHECK(stats.powerupTime == 1.25f);
    CHECK(stats.weaponOffset == Vec3{-0.5f, 0.5f, 1.5f});
    CHECK(stats.familiarOffset == Vec3{-1.0f, 5.0f, 0.75f});
    CHECK(stats.familiarShotOffset == Vec3{1.25f, 6.0f, -0.5f});
    CHECK(stats.streakForward == 2.5f);
    for (usize tier = 0; tier < ClassStats::kGlowTiers; ++tier) {
        const f32 base = static_cast<f32>(tier * 3) * 0.25f;
        CHECK(stats.weaponGlowOffsets[tier] == Vec3{base, base + 0.25f, base + 0.5f});
        CHECK(stats.weaponGlowScales[tier] == Vec3{base + 7.5f, base + 7.75f, base + 8.0f});
    }
    CHECK(stats.moves.turboB == 0);
    CHECK(stats.moves.comboHit == 0); // slot 10, not the unused combo2 in slot 9
    REQUIRE(stats.moveEffects.size() == 1);
    const auto& effect = stats.moveEffects.front();
    CHECK(effect.tree == "HAND_GLOW");
    CHECK(effect.sound == "R_WRIST");
    CHECK(effect.particle());
    CHECK(effect.next == -1);
    CHECK(effect.offset == Vec3{1.0f, 2.0f, 3.0f});
    CHECK(effect.scale == 1.25f);
    CHECK(effect.lifetime == 0.5f);
    CHECK(effect.radius == 3.0f);
    CHECK(effect.alphaMod == 150);
    CHECK(effect.color == 0x7F12AB34);
    CHECK(effect.tint() == Color::rgba(0x12, 0xAB, 0x34));
    REQUIRE(stats.moveStrikes.size() == 1);
    const auto& strike = stats.moveStrikes.front();
    CHECK(strike.type == MoveStrike::kFlies);
    CHECK(strike.hitRadius == 2.0f);
    CHECK(strike.radius == 3.0f);
    CHECK(strike.delay == 0.5f);
    CHECK(strike.maxTime == 6.0f);
    CHECK(strike.angle == -0.5f);
    CHECK(strike.arc == 0.75f);
    CHECK(strike.offset == Vec3{4.0f, 5.0f, 6.0f});
    CHECK(strike.amount == -2.0f);
    CHECK(strike.speed == 35.0f);
    CHECK(strike.damageType == 0x100021);
    CHECK(strike.effect == 0);
    CHECK(strike.hitEffect == 0);
    CHECK(strike.loopEffect == 0);
    CHECK(strike.next == -1);
    CHECK(strike.startFrame == 9);
    CHECK(strike.endFrame == 30);
    CHECK(strike.flags == 0x2010);
    CHECK(strike.help == 57);
}

TEST_CASE("native class files take precedence and failed reloads clear stale tuning",
          "[game][players][native-class]") {
    const auto dir = test::scratchDirectory("native-class-load");
    const auto pdata = dir / "PdAtA";
    std::filesystem::create_directories(pdata);
    writeFile(pdata / "wAr.WaD", nativeClassWad());
    constexpr auto kLegacy = R"({"fight":[1,2],"speed":[3,4],"armor":[5,6],"magic":[7,8]})";
    writeTextFile(pdata / "WAR.json", kLegacy);
    writeTextFile(pdata / "vAl.JsOn", kLegacy);
    ClassDataSet classes;
    REQUIRE(classes.load(dir / "pdata"));
    REQUIRE(classes.loadedCount() == 2);
    REQUIRE(classes.stats(0) != nullptr);
    CHECK(classes.stats(0)->fightMin == 600.0f);
    REQUIRE(classes.stats(1) != nullptr);
    CHECK(classes.stats(1)->fightMin == 1.0f);
    writeFile(pdata / "wAr.WaD", std::array<u8, 3>{1, 2, 3});
    REQUIRE(classes.load(pdata)); // the independent legacy-only class still loads
    CHECK(classes.loadedCount() == 1);
    CHECK(classes.stats(0) == nullptr); // never the stale WAR.json or previous native record
    const auto empty = test::scratchDirectory("native-class-empty");
    CHECK_FALSE(classes.load(empty));
    CHECK_FALSE(classes.loaded());
    CHECK(classes.stats(1) == nullptr);
}

TEST_CASE("native class loading rejects missing truncated and inconsistent tables",
          "[game][players][native-class]") {
    auto bytes = nativeClassWad();
    SECTION("missing effects") {
        bytes[kDirectoryOffset] = 'Z';
    }
    SECTION("missing strikes") {
        bytes[kDirectoryOffset + 16] = 'Z';
    }
    SECTION("missing class record") {
        bytes[kDirectoryOffset + 32] = 'Z';
    }
    SECTION("effect count disagrees with directory") {
        bytes[kClassOffset] = 2;
    }
    SECTION("strike count disagrees with directory") {
        bytes[kClassOffset + 2] = 2;
    }
    SECTION("strike table extends into class record") {
        bytes[kClassOffset + 2] = 2;
        bytes[kDirectoryOffset + 16 + 8] = 2;
    }
    SECTION("class table starts inside header") {
        bytes[kDirectoryOffset + 32 + 4] = 0;
    }
    SECTION("class table extends into directory") {
        ++bytes[kDirectoryOffset + 32 + 4];
    }
    SECTION("truncated directory") {
        bytes.pop_back();
    }
    REQUIRE_THROWS_AS(parseNativeClassStats(bytes), FormatError);
}

TEST_CASE("retail native class data agrees with every exported runtime field",
          "[game][players][native-class][assets][unpacked]") {
    const auto nativeDirectory = test::assetOrSkip("PDATA/WAR.WAD").parent_path();
    const char* reference = std::getenv("GDL_NATIVE_REFERENCE_DIR");
    if (reference == nullptr) {
        SKIP("Set GDL_NATIVE_REFERENCE_DIR to a fresh PDATA export for comparison");
    }
    const auto exportDirectory = std::filesystem::path(reference) / "pdata";
    REQUIRE(std::filesystem::exists(exportDirectory / "WAR.json"));
    ClassDataSet native;
    ClassDataSet exported;
    REQUIRE(native.load(nativeDirectory));
    REQUIRE(exported.load(exportDirectory));
    REQUIRE(native.loadedCount() == 16);
    REQUIRE(native.loadedCount() == exported.loadedCount());
    for (s32 index = 0; index < kClassCount; ++index) {
        CAPTURE(classCode(index));
        const auto* actual = native.stats(index);
        const auto* expected = exported.stats(index);
        REQUIRE((actual == nullptr) == (expected == nullptr));
        if (actual != nullptr) {
            sameClass(*actual, *expected);
        }
    }
}

} // namespace
