#include <algorithm>
#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/enemies/LegendsEnemySounds.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr usize kSoundTable = 0xe005fbb0U - 0xe0000400U;
void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16));
    put16(bytes, at + 2, static_cast<u16>(value));
}
std::vector<u8> program() {
    std::vector<u8> result(kSoundTable + 210);
    for (usize family = 0; family < 7; ++family)
        for (usize cue = 0; cue < 15; ++cue)
            put16(result, kSoundTable + (family * 15 + cue) * 2,
                  static_cast<u16>(family * 100 + cue));
    return result;
}
std::vector<u8> records() {
    std::vector<u8> result(8 + 52 + 4);
    put32(result, 0, 8);
    put32(result, 4, 52);
    put32(result, 8, 0x54524f4c);
    result[8 + 4 + 39] = 3;
    return result;
}
TEST_CASE("N64 enemy cries use native family and post-hit health thresholds",
          "[legends][native-enemy-sounds]") {
    const auto sounds = LegendsEnemySounds::decode(program(), records());
    EnemyFeedback hit;
    hit.kind = 1;
    CHECK(sounds.feedback(hit, 100) == 306);
    hit.tier = 2;
    CHECK(sounds.feedback(hit, 7) == 307);
    CHECK(sounds.feedback(hit, 7.01f) == 308);
    hit.tier = 3;
    CHECK(sounds.feedback(hit, 5) == 309);
    CHECK(sounds.feedback(hit, 5.01f) == 310);
    CHECK(sounds.feedback(hit, 10) == 310);
    CHECK(sounds.feedback(hit, 10.01f) == 311);
    hit.killed = true;
    for (s32 tier = 1; tier <= 3; ++tier) {
        hit.tier = tier;
        CHECK(sounds.feedback(hit, -20) == static_cast<u32>(311 + tier));
    }
    hit.kind = 2;
    CHECK_FALSE(sounds.feedback(hit, 1));
    hit.kind = 1;
    CHECK_FALSE(sounds.feedback(hit, std::numeric_limits<f32>::quiet_NaN()));
    auto muted = program();
    put16(muted, kSoundTable + (3 * 15 + 14) * 2, 0xffff);
    CHECK_FALSE(LegendsEnemySounds::decode(muted, records()).feedback(hit, -1));
}
TEST_CASE("N64 enemy melee slots preserve authored silence and exact tier selection",
          "[legends][native-enemy-sounds]") {
    auto exe = program();
    put16(exe, kSoundTable + (3 * 15 + 4) * 2, 0xffff);
    const auto sounds = LegendsEnemySounds::decode(exe, records());
    for (s32 tier = 1; tier <= 6; ++tier) {
        if (tier == 5)
            CHECK_FALSE(sounds.melee(1, tier));
        else
            CHECK(sounds.melee(1, tier) == static_cast<u32>(299 + tier));
    }
    CHECK_FALSE(sounds.melee(-1, 1));
    CHECK_FALSE(sounds.melee(34, 1));
    CHECK_FALSE(sounds.melee(2, 1));
    CHECK_FALSE(sounds.melee(1, 0));
    CHECK_FALSE(sounds.melee(1, 7));
}
TEST_CASE("N64 enemy sound tables reject truncated malformed or duplicate records",
          "[legends][native-enemy-sounds]") {
    auto exe = program();
    auto enemies = records();
    SECTION("short executable") {
        exe.pop_back();
    }
    SECTION("short record") {
        enemies.resize(8 + 51);
    }
    SECTION("bad stride") {
        put32(enemies, 4, 51);
    }
    SECTION("bad family") {
        enemies[51] = 7;
    }
    SECTION("duplicate identity") {
        enemies.resize(8 + 104 + 4);
        put32(enemies, 60, 0x54524f4c);
    }
    CHECK_THROWS_AS(LegendsEnemySounds::decode(exe, enemies), FormatError);
}
TEST_CASE("Installed enemy voices retain original seven-family numeric identities",
          "[legends][native-enemy-sounds][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "enemy.rom"))
        SKIP("N64 native enemy table is not installed");
    const auto sounds = LegendsEnemySounds::decode(readFile(root / "_program/game.bin"),
                                                   readFile(root / "enemy.rom"));
    for (s32 kind : {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 29, 30, 31}) {
        for (s32 tier = 1; tier <= 3; ++tier) {
            CAPTURE(kind, tier);
            EnemyFeedback hit{.kind = kind, .tier = tier};
            REQUIRE(sounds.feedback(hit, 20));
            hit.killed = true;
            REQUIRE(sounds.feedback(hit, 0));
        }
    }
    EnemyFeedback troll{.kind = 1, .tier = 3};
    CHECK(sounds.feedback(troll, 4) == 0xcb);
    CHECK(sounds.feedback(troll, 8) == 0xcc);
    CHECK(sounds.feedback(troll, 11) == 0xcd);
    troll.killed = true;
    CHECK(sounds.feedback(troll, 0) == 0xce);
    CHECK(sounds.melee(1, 1) == 0x95);
    CHECK(sounds.melee(0, 1) == 0x72);
    CHECK(sounds.melee(3, 1) == 0x69);
    CHECK(sounds.melee(15, 1) == 0x11e);
    CHECK(sounds.melee(15, 2) == 0x11f);
    CHECK(sounds.melee(29, 3) == 0x49);
    CHECK_FALSE(sounds.melee(1, 4));
}

TEST_CASE("Native enemy cries and Death loops reach the shared mixer without donor audio",
          "[legends][native-enemy-sounds][native-enemy-playback][audio][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("Native N64 extraction is not installed");
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    LegendsLevel level(device, map, std::move(adapter), directory, characters);
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer output(mixer);
    SoundPlayer reference(referenceMixer);
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    u32 expected = 0;
    bool draining = false;
    SECTION("hurt uses the captured remaining health") {
        REQUIRE(level.playEnemyFeedback(EnemyFeedback{.kind = 1, .tier = 3}, 8));
        expected = 0xcc;
    }
    SECTION("melee uses the native bite family") {
        REQUIRE(level.playEnemyMelee(15, 2));
        expected = 0x11f;
    }
    SECTION("Death leaves") {
        REQUIRE(level.playDeathCue(DeathEvent{.kind = DeathEvent::Kind::Exhausted}));
        expected = 0x2b;
    }
    SECTION("suicide burst uses the native explosion") {
        REQUIRE(level.playOpponentSound("S_SUICIDE_BOMB", Vec3{10000}));
        expected = 0x93;
    }
    SECTION("lobber impact uses its native contact cue") {
        REQUIRE(level.playOpponentSound("S_LOBBER_BOMB", Vec3{10000}));
        expected = 0x57;
    }
    SECTION("Death drain is a single retained native macro") {
        REQUIRE(level.setDeathContact(true));
        expected = 1;
        draining = true;
    }
    REQUIRE(expected != 0);
    REQUIRE(sounds.play(reference, expected) != kNoSound);
    std::array<f32, 1600> actual{}, original{};
    f64 energy = 0;
    for (usize tick = 0; tick < 60; ++tick) {
        CAPTURE(tick, expected);
        // No authored ambient zone reaches this listener. Enemy cries and the
        // drain are native global cues and must not receive camera attenuation.
        level.playSounds(output, Vec3{10000});
        output.update();
        reference.update();
        mixer.mix(actual);
        referenceMixer.mix(original);
        REQUIRE(actual == original);
        for (const auto sample : actual)
            energy += static_cast<f64>(sample) * sample;
    }
    CHECK(energy > .001);
    if (draining) {
        REQUIRE(output.voiceCount() == 1);
        level.pauseSounds();
        mixer.mix(actual);
        output.update();
        mixer.mix(actual);
        CHECK(output.voiceCount() == 0);
        CHECK(std::ranges::all_of(actual, [](f32 value) { return value == 0; }));
        level.playSounds(output, Vec3{10000});
        CHECK(output.voiceCount() == 1);
        REQUIRE(level.setDeathContact(false));
    }
    level.stopSounds();
    mixer.mix(actual);
    output.update();
    mixer.mix(actual);
    CHECK(output.voiceCount() == 0);
    level.playSounds(output, Vec3{10000});
    CHECK(output.voiceCount() == 0);
    level.stopSounds();
}
} // namespace
