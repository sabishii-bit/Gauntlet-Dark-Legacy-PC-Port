#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBosses.h"
#include "game/bosses/Bosses.h"
#include "game/combat/EffectFlags.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"
#include "game/level/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto path = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(path / "_program/Boss.bin")) {
        SKIP("Native N64 Legends boss overlay is not installed");
    }
    return path;
}

struct Expected {
    std::string_view map;
    std::string_view archive;
    std::string_view donor;
    s32 kind;
    Vec3 position;
    u8 shard;
};

const std::array kExpected{
    Expected{"mount6", "dragon", "B6", 34, {-4, 47, -16}, 1},
    Expected{"ice5", "yeti", "I5", 39, {10, -5, -83}, 2},
    Expected{"town5", "slime", "K5", 38, {0, 0, 0}, 4},
    Expected{"castle5", "chi", "A5", 35, {0, 20, -52}, 3},
    Expected{"boss2", "sko1", "E2", 42, {0, -20, -8}, 0},
    Expected{"hell2", "sko2", "F2", 43, {0, -34, -8}, 0},
};

TEST_CASE("Native boss camera reads attention separately from the combat origin",
          "[legends][bosses][camera]") {
    constexpr usize kType = 8;
    constexpr usize kDirectory = kType + 0xf8;
    std::vector<u8> bytes(kDirectory + 12);
    const auto word = [&](usize at, u32 value) {
        for (usize i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<u8>(value >> (24U - static_cast<u32>(i) * 8U));
        }
    };
    word(0, kDirectory);
    word(4, 1);
    word(kDirectory, 0x54595045);
    word(kDirectory + 4, kType);
    word(kDirectory + 8, 1);
    word(kType + 0x8c, std::bit_cast<u32>(12.0f));
    word(kType + 0x94, std::bit_cast<u32>(-3.0f));
    word(kType + 0x98, std::bit_cast<u32>(27.0f));
    word(kType + 0x9c, std::bit_cast<u32>(99.0f));
    const auto camera = legendsN64BossCamera(bytes);
    CHECK(camera.floorOffset == -3);
    CHECK(camera.attentionHeight == 27);
    CHECK(camera.radius == 12);
    word(kType + 0x98, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
    CHECK_THROWS_AS(legendsN64BossCamera(bytes), FormatError);
    word(kType + 0x98, 0);
    word(kDirectory + 8, 2); // Count may not read into the WAD directory.
    CHECK_THROWS_AS(legendsN64BossCamera(bytes), FormatError);
    CHECK_THROWS_AS(legendsN64BossCamera({}), FormatError);
}

TEST_CASE("Every native boss family has finite authored camera metadata",
          "[legends][bosses][camera][assets]") {
    const auto root = nativeRoot();
    for (const auto& expected : kExpected) {
        CAPTURE(expected.archive);
        const auto camera = legendsN64BossCamera(readFile(root / expected.archive / "moves.wad"));
        CHECK(std::isfinite(camera.floorOffset));
        CHECK(std::isfinite(camera.attentionHeight));
        CHECK(camera.radius > 0);
    }
}

TEST_CASE("Legends boss script rejects unsupported executables before fixed-address reads",
          "[legends][bosses]") {
    CHECK_FALSE(ImportedBoss{}.rangedAimFirst);
    CHECK_FALSE(legendsN64Boss("../mount6", {}, {}));
    CHECK_THROWS_AS(legendsN64Boss("mount6", {}, {}), FormatError);
    const std::array<u8, 64> truncated{};
    CHECK_THROWS_AS(legendsN64Boss("mount6", truncated, truncated), FormatError);
}

TEST_CASE("native Legends boss stages preserve placements and deferred pickup rewards",
          "[legends][bosses][assets]") {
    const auto root = nativeRoot();
    const auto program = readFile(root / "_program/game.bin");
    auto overlay = readFile(root / "_program/Boss.bin");
    for (const auto& expected : kExpected) {
        CAPTURE(expected.map);
        const auto encounter = legendsN64Boss(expected.map, program, overlay);
        REQUIRE(encounter);
        CHECK(encounter->map == expected.map);
        CHECK(encounter->nativeArchive == expected.archive);
        CHECK(encounter->combat.kind == expected.kind);
        CHECK(encounter->combat.rangedAimFirst);
        CHECK(encounter->combat.counterpartStage == expected.donor);
        CHECK(encounter->combat.position == expected.position);
        CHECK(encounter->combat.yaw == Catch::Approx(expected.kind == 34 ? -0.209439516f : 0));
        CHECK(encounter->destination == "tower2");
        const usize rockCount = expected.kind == 34   ? 6
                                : expected.kind == 38 ? 3
                                : expected.kind == 39 ? 8
                                                      : 0;
        REQUIRE(encounter->combat.arenaRocks.size() == rockCount);
        for (const auto& rock : encounter->combat.arenaRocks) {
            CHECK(rock.radius == 2.3f);
            CHECK(rock.height == 5);
            CHECK(rock.tierHealth == (expected.kind == 38 ? 80 : 40));
        }
        CHECK(std::filesystem::is_regular_file(root / "levels" / expected.map / "worlds.wad"));
        CHECK(std::filesystem::is_regular_file(root / expected.archive / "moves.wad"));
        if (expected.shard != 0) {
            REQUIRE(encounter->rewards.size() == 1);
            CHECK(encounter->rewards[0].kind == 0x2b);
            CHECK(encounter->rewards[0].subtype == expected.shard);
            CHECK(encounter->rewards[0].angle == 0);
            CHECK(encounter->sharedObelisk == -1);
            CHECK(encounter->departureSeconds == 12);
        } else if (expected.kind == 42) {
            REQUIRE(encounter->rewards.size() == 4);
            for (usize i = 0; i < 4; ++i) {
                CHECK(encounter->rewards[i].kind == 0x2a);
                CHECK(encounter->rewards[i].subtype == i + 1);
                CHECK(encounter->rewards[i].distance == 30);
            }
            CHECK(encounter->sharedObelisk == 7);
            CHECK(encounter->consumedLegend == -1);
            CHECK(encounter->departureSeconds == 12);
        } else {
            CHECK(encounter->rewards.empty());
            CHECK(encounter->sharedObelisk == -1);
            CHECK(encounter->departureSeconds == 5.5f);
            CHECK(encounter->clearsRunestones);
            CHECK(encounter->finalNativeGrant == 0x300290U);
        }
    }
    for (const char* const map : {"boss1", "hell1", "tower2", "mount5", "ice6", "unknown"}) {
        CAPTURE(map);
        CHECK_FALSE(legendsN64Boss(map, program, overlay));
    }
    const auto upper = legendsN64Boss("MOUNT6", program, overlay);
    REQUIRE(upper);
    CHECK(upper->map == "mount6");
    overlay[0x9c04] ^= 1;
    CHECK_THROWS_AS(legendsN64Boss("mount6", program, overlay), FormatError);
}

TEST_CASE("an N64 boss's focus, which its rewards fly about, is its floor raised by its root's "
          "height and its focus's over the root",
          "[legends][bosses]") {
    // e0884a9c stands the root TYPE+0x94 over the floor its probe finds (e0884cfc-e0884d0c) and
    // publishes the root raised by TYPE+0x98 (e0884d60-e0884dac); the combat origin's TYPE+0x9c
    // and the camera's radius play no part.
    const LegendsBossCamera type{.floorOffset = -3, .attentionHeight = 27, .radius = 12};
    CHECK(legendsN64BossFocus(type, Vec3{4, 10, -6}) == Vec3{4, 34, -6});
    CHECK(legendsN64BossFocus({}, Vec3{4, 10, -6}) == Vec3{4, 10, -6});
}

TEST_CASE("each shipped N64 boss's focus stands its own height over its floor",
          "[legends][bosses][camera][assets]") {
    const auto root = nativeRoot();
    struct Focus {
        std::string_view archive;
        f32 height; ///< TYPE+0x94 plus TYPE+0x98
    };
    constexpr std::array kFocus{Focus{"dragon", 13}, Focus{"yeti", 10}, Focus{"slime", 9},
                                Focus{"chi", 9},     Focus{"sko1", 20}, Focus{"sko2", 33}};
    for (const auto& expected : kFocus) {
        CAPTURE(expected.archive);
        const auto type = legendsN64BossCamera(readFile(root / expected.archive / "moves.wad"));
        CHECK(legendsN64BossFocus(type, Vec3{0}) == Vec3{0, expected.height, 0});
    }
}

TEST_CASE("native Legends boss reward fan retains vertical direction and launch velocity",
          "[legends][bosses]") {
    const LegendsBossReward reward{0x2b, 1, 0, 50};
    const auto motion = legendsBossRewardMotion(reward, {0, -20, 0}, {0, 0, 30});
    REQUIRE(motion);
    const Vec3 direction = glm::normalize(Vec3{0, -10, 30});
    const Vec3 target = Vec3{0, 10, 0} + direction * 50.0f;
    const Vec3 origin = direction * 10.0f + Vec3{0, 5, 0};
    const f32 duration = glm::length(target - origin) / 20;
    CHECK(glm::length(motion->position - origin) < 0.0001f);
    CHECK(glm::length(motion->velocity -
                      ((target - origin) / duration + Vec3{0, 32 * duration, 0})) < 0.0001f);
    CHECK(glm::length(motion->position + motion->velocity * duration -
                      Vec3{0, 32 * duration * duration, 0} - target) < 0.0001f);
    const auto turned = legendsBossRewardMotion({0x2a, 1, kHalfPi, 30}, {0, 0, 0}, {0, 0, 30});
    REQUIRE(turned);
    CHECK(turned->position.x < -9);
    CHECK(std::abs(turned->position.z) < 0.0001f);
    CHECK_FALSE(legendsBossRewardMotion(reward, {0, std::numeric_limits<f32>::infinity(), 0}, {}));
    CHECK_FALSE(legendsBossRewardMotion({0, 0, 0, -1}, {}, {}));
    CHECK_FALSE(legendsBossRewardMotion({0, 0, std::numeric_limits<f32>::quiet_NaN(), 1}, {}, {}));
}

TEST_CASE("native boss reward templates preserve script identity and collectable metadata",
          "[legends][bosses][assets]") {
    const auto root = nativeRoot();
    const auto program = readFile(root / "_program/game.bin");
    const auto overlay = readFile(root / "_program/Boss.bin");
    const auto powerups = readFile(root / "items/generic/ppow.rom");
    for (const auto& expected : kExpected) {
        CAPTURE(expected.map);
        const auto encounter = legendsN64Boss(expected.map, program, overlay);
        REQUIRE(encounter);
        const auto fixtures = legendsN64BossRewardFixtures(*encounter, powerups);
        REQUIRE(fixtures.size() == encounter->rewards.size());
        const auto archives = formats::legendsN64FixtureArchives(expected.map, fixtures);
        for (usize i = 0; i < fixtures.size(); ++i) {
            const auto& fixture = fixtures[i];
            CHECK(fixture.nativeIndex == 0x80000000U + i);
            CHECK_FALSE(fixture.groundProjected);
            CHECK(fixture.group == "POWERUP");
            CHECK(fixture.sequence.size() == 4);
            CHECK(fixture.templateKey == (static_cast<u16>(encounter->rewards[i].kind) << 8U |
                                          encounter->rewards[i].subtype));
            CHECK(fixture.resolvedTemplateKey == fixture.templateKey);
            CHECK(std::ranges::find(archives, fixture.archive) != archives.end());
        }
        for (const auto& archive : archives) {
            CHECK(std::filesystem::is_regular_file(root / archive / "seq.wad"));
        }
        if (!fixtures.empty()) {
            CHECK_THROWS_AS(legendsN64BossRewardFixtures(*encounter, {}), FormatError);
            CHECK_THROWS_AS(legendsN64BossRewardFixtures(*encounter, std::span{powerups}.first(12)),
                            FormatError);
        }
    }
    LegendsBossEncounter invalid;
    invalid.rewards.push_back({0xff, 1, 0, 1});
    CHECK_THROWS_AS(legendsN64BossRewardFixtures(invalid, powerups), FormatError);
    invalid.rewards.assign(5, LegendsBossReward{0x2b, 1, 0, 1});
    CHECK_THROWS_AS(legendsN64BossRewardFixtures(invalid, powerups), FormatError);
}

TEST_CASE("all six Legends boss counterparts bind their real GDL stage textures and audio",
          "[legends][bosses][assets]") {
    const auto native = nativeRoot();
    const auto program = readFile(native / "_program/game.bin");
    const auto overlay = readFile(native / "_program/Boss.bin");
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    SoundSet common;
    SoundSet ambient;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    REQUIRE(ambient.load(root / "AUDIO/TOWAMB"));
    AnimationSet shared;
    REQUIRE(shared.load(root / "WEAPONS"));
    for (const auto& expected : kExpected) {
        CAPTURE(expected.map);
        const auto encounter = legendsN64Boss(expected.map, program, overlay);
        REQUIRE(encounter);
        const auto& definition = encounter->combat;
        const auto donor = catalog.byName(definition.counterpartStage);
        REQUIRE(donor);
        CHECK(donor->realm == definition.counterpartWorld);
        WorldData world;
        REQUIRE(world.load(root / "WDATA" / (definition.counterpartWorld + ".WAD")));
        const auto level =
            std::ranges::find(world.levels(), definition.counterpartStage, &LevelInfo::name);
        REQUIRE(level != world.levels().end());
        CHECK(level->bossType == definition.kind);
        const auto* audio = world.audio(level->audioIndex);
        REQUIRE(audio);
        CHECK(audio->bank == definition.soundBank);
        SoundSet bank;
        REQUIRE(bank.load(root / "AUDIO" / definition.soundBank));
        test::FakeRenderDevice device;
        TextureSet stage;
        REQUIRE(stage.load(root / donor->directory));
        const std::array lenders{&stage};
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, bossDefinition(bossNameOf(definition.kind)),
                            definition.counterpartStage.front(), lenders));
        CHECK_FALSE(assets.data.movement().home);
        for (const auto& effect : assets.data.sounds()) {
            const auto cue = effect.soundFor(definition.counterpartStage.front());
            CAPTURE(cue, effect.tree);
            CHECK((cue.empty() || bank.find(cue) || common.find(cue) || ambient.find(cue)));
            if (effect.shows() && (effect.flags & effect_flags::kSkin) != 0) {
                REQUIRE(assets.skins.contains(effect.tree));
                CHECK_FALSE(assets.skins.at(effect.tree).empty());
            }
        }
        Bosses boss;
        boss.open(device, root, nullptr, {}, definition.counterpartStage.front(), lenders);
        REQUIRE(boss.spawn(definition.kind, definition.position, definition.yaw));
        CHECK(boss.present());
        CHECK(boss.view().alive);
        CHECK(boss.view().kind == definition.kind);
        CHECK(boss.view().maxHealth > 0);
        REQUIRE(boss.position());
        CHECK(*boss.position() == definition.position);
        boss.close();
    }
}

} // namespace
