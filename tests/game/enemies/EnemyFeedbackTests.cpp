#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"

#include "TestSupport.h"
#include "game/enemies/EnemyFeedback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("enemy melee selects bite strike or tiered player impacts without generic duplicates",
          "[enemy-feedback][enemy-melee]") {
    // GUNE5D 8004DF58 chooses damage mode zero for these contacts. AudioPlayerHit's
    // rows 3/4 are COMMON samples 0x35/0x36; AudioSetupBossStreams builds BITE/STRIKE.
    const std::array roster{LevelEnemy{3, 1, "RAT"}, LevelEnemy{2, 1, "DEM"},
                            LevelEnemy{8, 12, "MUM"}, LevelEnemy{11, 1, "TRE"}};
    for (const s32 kind : {4, 5, 10}) {
        CHECK(EnemyFeedback::meleeSound(kind, 1, {}) == "S_PLYRDMG5");
        CHECK(EnemyFeedback::meleeSound(kind, 2, {}) == "S_PLYRDMG4");
        CHECK(EnemyFeedback::meleeSound(kind, 3, {}) == "S_PLYRDMG4");
    }
    CHECK(EnemyFeedback::meleeSound(3, 3, roster) == "S_RATBITE");
    CHECK(EnemyFeedback::meleeSound(2, 1, roster) == "S_DEM1BITE");
    CHECK(EnemyFeedback::meleeSound(2, 3, roster) == "S_DEM2BITE");
    CHECK(EnemyFeedback::meleeSound(8, 1, roster) == "S_MUM2BITE");
    CHECK(EnemyFeedback::meleeSound(11, 1, roster) == "S_TRE1STRIKE");
    CHECK(EnemyFeedback::meleeSound(11, 2, roster) == "S_TRE2STRIKE");
    CHECK(EnemyFeedback::meleeSound(2, 1, roster, 41) == "S_DEM2BITEB");
    REQUIRE(EnemyFeedback::meleeSound(3, 1, {}).has_value());
    CHECK(EnemyFeedback::meleeSound(3, 1, {})->empty());
    for (const s32 kind : {-1, 1, 7, 13, 27, 31, 100}) {
        CHECK_FALSE(EnemyFeedback::meleeSound(kind, 1, roster).has_value());
    }
}

TEST_CASE("retail enemy feedback resolves every gameplay realm's audio roster",
          "[enemy-feedback][unpacked]") {
    const auto root = test::unpackedOrSkip("wdata/TEMPLE.json").parent_path().parent_path();
    // TEST is developer content with placeholder TOWN audio, not a gameplay realm.
    constexpr std::array kRealms{"TOWN",  "MOUNT",  "CASTLE", "FOREST", "DESERT", "ICE",
                                 "DREAM", "BATTLE", "SKY",    "TEMPLE", "HELL",   "SECRET"};
    usize checked = 0;
    for (const auto* realm : kRealms) {
        test::unpackedOrSkip(std::string("wdata/") + realm + ".json");
        WorldData world;
        REQUIRE(world.load(root / "wdata" / (std::string(realm) + ".json")));
        for (const auto& level : world.levels()) {
            const auto* audio = world.audio(level.audioIndex);
            REQUIRE(audio != nullptr);
            test::unpackedOrSkip("audio/" + audio->bank + "/sounds.json");
            SoundSet bank;
            REQUIRE(bank.load(root / "audio" / audio->bank));
            SoundSet common;
            REQUIRE(common.load(test::unpackedOrSkip("audio/COMMON/sounds.json").parent_path()));
            for (const auto& enemy : level.enemies) {
                if (enemy.kind >= 28 || enemy.stream.empty()) {
                    continue;
                }
                for (s32 tier = 1; tier <= 3; ++tier) {
                    if (const auto impact = EnemyFeedback::meleeSound(
                            enemy.kind, tier, level.enemies, level.bossType)) {
                        INFO(realm << "/" << level.name << ": melee " << *impact);
                        CHECK((bank.find(*impact).has_value() || common.find(*impact).has_value()));
                    }
                    for (const bool close : {false, true}) {
                        for (const bool killed : {false, true}) {
                            for (s32 hits = 1; hits <= 2; ++hits) {
                                EnemyFeedback feedback;
                                feedback.kind = enemy.kind;
                                feedback.tier = tier;
                                feedback.close = close;
                                feedback.killed = killed;
                                feedback.hitCount = hits;
                                const auto name = feedback.sound(level.enemies, level.bossType);
                                INFO(realm << "/" << level.name << ": " << name);
                                const auto id = bank.find(name);
                                REQUIRE(id);
                                CHECK_FALSE(bank.sequence(*id).steps.empty());
                                ++checked;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(checked > 1000);
}

TEST_CASE("enemy sounds distinguish tier hit count and close versus far damage",
          "[game][enemies][enemy-feedback]") {
    const std::array roster{LevelEnemy{13, 2, "EGRUNT"}, LevelEnemy{27, 2, "GRM"}};
    EnemyFeedback feedback;
    feedback.kind = 13;
    CHECK(feedback.sound(roster) == "S_EGRUNT1HITFAR");
    feedback.close = true;
    CHECK(feedback.sound(roster) == "S_EGRUNT1HITCLO");
    feedback.tier = 2;
    CHECK(feedback.sound(roster) == "S_EGRUNT2HIT1CL");
    feedback.hitCount = 2;
    CHECK(feedback.sound(roster) == "S_EGRUNT2HIT2CL");
    feedback.killed = true;
    CHECK(feedback.sound(roster) == "S_EGRUNT2DIECLO");
    feedback.close = false;
    CHECK(feedback.sound(roster) == "S_EGRUNT2DIEFAR");
    feedback.tier = 1;
    CHECK(feedback.sound(roster, 41) == "S_EGRUNT2DIEFAB");
    feedback.kind = 27;
    CHECK(feedback.sound(roster) == "S_GRM1DIEFAR");
    feedback.kind = -1;
    CHECK(feedback.sound(roster).empty());
    feedback.kind = 13;
    CHECK(feedback.sound({}).empty());
    const std::array temple{LevelEnemy{13, 12, "EGRUNT"}};
    CHECK(feedback.sound(temple) == "S_EGRUNT2DIEFAR");
}

TEST_CASE("enemy impact and death skins retain elemental and nonflesh distinctions",
          "[game][enemies][enemy-feedback]") {
    EnemyFeedback feedback;
    feedback.kind = 13;
    feedback.halfHeight = 4;
    CHECK(feedback.effect() == "BLOODFX1");
    CHECK(feedback.effectScale() == Approx(2));
    CHECK(feedback.deathSkin() == "DEATHBLOOD");
    CHECK(feedback.deathSkinFrames() == 10);
    feedback.killed = true;
    CHECK(feedback.effect() == "BLOODFX2");
    feedback.flags = 1;
    CHECK(feedback.effect() == "FIREDIE");
    CHECK(feedback.deathSkin() == "DEATHFIRE");
    feedback.flags = 2;
    CHECK(feedback.effect() == "ELECDIE");
    CHECK(feedback.deathSkin() == "DEATHELEC");
    feedback.flags = 3;
    CHECK(feedback.effect() == "LIGHTDIE");
    feedback.flags = 4;
    CHECK(feedback.effect() == "ACIDDIE");
    feedback.flags = 0;
    feedback.kind = 5;
    CHECK(feedback.effect() == "HITDIE");
    CHECK(feedback.effectScale() == Approx(1));
    CHECK(feedback.deathSkin() == "DEATHALT");
    CHECK(feedback.deathSkinFrames() == 15);
    feedback.kind = 11;
    CHECK(feedback.deathSkinFrames() == 10);
    CHECK(feedback.effect() == "TREEDIE");
    feedback.killed = false;
    CHECK(feedback.effect() == "TREEHIT");
    feedback.kind = 21;
    feedback.halfHeight = 1;
    CHECK(feedback.effect() == "TREEHIT");
    CHECK(feedback.deathSkin().empty());
    CHECK(feedback.deathSkinFrames() == 0);
    feedback.flags = 0x1000000;
    CHECK(feedback.effect().empty());
    CHECK_FALSE(feedback.sound(std::array{LevelEnemy{21, 1, "ACI"}}).empty());
    feedback.flags = 15;
    CHECK(feedback.effect().empty());
    CHECK(feedback.deathSkin().empty());
}
} // namespace
