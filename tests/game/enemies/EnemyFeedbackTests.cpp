#include <array>
#include <limits>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/EnemyKinds.h"

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

TEST_CASE("only the boss codes with a bank variant letter change a swarm bank's name",
          "[game][enemies][enemy-feedback][boss-kinds]") {
    // A large grunt's death far off and a large demon's bite: byte 14 of a bank key is the
    // boss level's variant letter, where the boss's code has one.
    const std::array roster{LevelEnemy{13, 2, "EGRUNT"}, LevelEnemy{2, 1, "DEM"}};
    EnemyFeedback dying;
    dying.kind = 13;
    dying.tier = 2;
    dying.killed = true;
    struct Row {
        s32 code;
        std::string_view death;
        std::string_view bite;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {33, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {34, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {35, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {36, "S_EGRUNT2DIEFAC", "S_DEM2BITEC"},
        {37, "S_EGRUNT2DIEFAD", "S_DEM2BITED"},
        {38, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {39, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {40, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {41, "S_EGRUNT2DIEFAB", "S_DEM2BITEB"},
        {42, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {43, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {44, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {45, "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
        {std::numeric_limits<s32>::min(), "S_EGRUNT2DIEFAR", "S_DEM2BITE"},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(dying.sound(roster, row.code) == row.death);
        CHECK(EnemyFeedback::meleeSound(2, 2, roster, row.code) == row.bite);
    }
}

TEST_CASE("retail enemy feedback resolves every gameplay realm's audio roster",
          "[enemy-feedback][assets]") {
    const auto root = test::assetOrSkip("WDATA/TEMPLE.WAD").parent_path().parent_path();
    // TEST is developer content with placeholder TOWN audio, not a gameplay realm.
    constexpr std::array kRealms{"TOWN",  "MOUNT",  "CASTLE", "FOREST", "DESERT", "ICE",
                                 "DREAM", "BATTLE", "SKY",    "TEMPLE", "HELL",   "SECRET"};
    usize checked = 0;
    for (const auto* realm : kRealms) {
        test::assetOrSkip(std::string("wdata/") + realm + ".WAD");
        WorldData world;
        REQUIRE(world.load(root / "wdata" / (std::string(realm) + ".WAD")));
        for (const auto& level : world.levels()) {
            const auto* audio = world.audio(level.audioIndex);
            REQUIRE(audio != nullptr);
            test::assetOrSkip("AUDIO/AUDATPS2.ROM");
            SoundSet bank;
            REQUIRE(bank.load(root / "audio" / audio->bank));
            SoundSet common;
            REQUIRE(common.load(test::assetOrSkip("audio/COMMON.vbk")));
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
TEST_CASE("each swarm kind keeps its hit and melee sound names at both tiers",
          "[game][enemies][enemy-traits]") {
    // A one-row roster per kind, struck far off and not killed on the first hit. "none" is
    // no dedicated melee sound: the player's ordinary pain answers instead.
    struct Row {
        s32 kind;
        std::string_view small;
        std::string_view large;
        std::string_view meleeSmall;
        std::string_view meleeLarge;
    };
    constexpr std::array<Row, 28> kRows{{
        {0, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {1, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {2, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_QQ1BITE", "S_QQ2BITE"},
        {3, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {4, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_PLYRDMG5", "S_PLYRDMG4"},
        {5, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_PLYRDMG5", "S_PLYRDMG4"},
        {6, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {7, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {8, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_QQ1BITE", "S_QQ2BITE"},
        {9, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {10, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_PLYRDMG5", "S_PLYRDMG4"},
        {11, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "S_QQ1STRIKE", "S_QQ2STRIKE"},
        {12, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {13, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {14, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {15, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {16, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {17, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {18, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {19, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {20, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {21, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {22, "S_QQHITFAR", "S_QQHITFAR", "S_QQBITE", "S_QQBITE"},
        {23, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {24, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {25, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {26, "S_QQ1HITFAR", "S_QQ2HIT1FAR", "none", "none"},
        {27, "S_QQ1HITFAR", "S_QQ1HITFAR", "none", "none"},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.kind);
        const std::array roster{LevelEnemy{row.kind, 1, "QQ"}};
        EnemyFeedback feedback;
        feedback.kind = row.kind;
        feedback.tier = 1;
        CHECK(feedback.sound(roster) == row.small);
        feedback.tier = 2;
        CHECK(feedback.sound(roster) == row.large);
        CHECK(EnemyFeedback::meleeSound(row.kind, 1, roster).value_or("none") == row.meleeSmall);
        CHECK(EnemyFeedback::meleeSound(row.kind, 2, roster).value_or("none") == row.meleeLarge);
    }
}

TEST_CASE("each kind keeps its marks and death skins with and without an element",
          "[game][enemies][enemy-traits]") {
    // Struck by no element and by fire, hurt and killed, on a body of the kind's own reach,
    // half its height, as Enemies hands it over. A skin and its frames are the same hurt or
    // killed.
    struct Row {
        s32 kind;
        std::string_view hit;
        std::string_view killed;
        std::string_view fireHit;
        std::string_view fireKilled;
        f32 scale;
        std::string_view skin;
        s32 frames;
        std::string_view fireSkin;
        s32 fireFrames;
    };
    constexpr std::array<Row, 47> kRows{{
        {-1, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {0, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {1, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {2, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {3, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {4, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {5, "HITCOL", "HITDIE", "FIREHIT", "FIREDIE", 1.0f, "DEATHALT", 15, "DEATHFIRE", 10},
        {6, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {7, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {8, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {9, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {10, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {11, "TREEHIT", "TREEDIE", "FIREHIT", "FIREDIE", 1.5f, "DEATHALT", 10, "DEATHFIRE", 10},
        {12, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {13, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {14, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {15, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {16, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {17, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {18, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {19, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {20, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {21, "TREEHIT", "TREEDIE", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {22, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 0.75f, "", 0, "", 0},
        {23, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {24, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {25, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {26, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {27, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 2.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {28, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", -0.25f, "", 0, "", 0},
        {29, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 3.0f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {30, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {31, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.25f, "DEATHBLOOD", 10, "DEATHFIRE",
         10},
        {32, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 2.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {33, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {34, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {35, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {36, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {37, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {38, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {39, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {40, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {41, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {42, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {43, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {44, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
        {45, "BLOODFX1", "BLOODFX2", "FIREHIT", "FIREDIE", 1.5f, "DEATHBLOOD", 10, "DEATHFIRE", 10},
    }};
    for (const Row& row : kRows) {
        const auto strike = [&row](u32 flags, bool killed, std::string_view mark,
                                   std::string_view skin, s32 frames) {
            CAPTURE(row.kind, flags, killed);
            EnemyFeedback feedback;
            feedback.kind = row.kind;
            feedback.halfHeight = 0.5f * enemyKind(row.kind).height;
            feedback.flags = flags;
            feedback.killed = killed;
            CHECK(feedback.effect() == mark);
            CHECK(feedback.effectScale() == row.scale);
            CHECK(feedback.deathSkin() == skin);
            CHECK(feedback.deathSkinFrames() == frames);
        };
        strike(0, false, row.hit, row.skin, row.frames);
        strike(0, true, row.killed, row.skin, row.frames);
        strike(damage::kFire, false, row.fireHit, row.fireSkin, row.fireFrames);
        strike(damage::kFire, true, row.fireKilled, row.fireSkin, row.fireFrames);
    }
}

} // namespace
