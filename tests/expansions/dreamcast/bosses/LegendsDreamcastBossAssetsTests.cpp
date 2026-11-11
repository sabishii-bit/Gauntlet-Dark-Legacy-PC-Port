#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/math/ApproximateLength.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBosses.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "game/bosses/Bosses.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Dreamcast boss loading never falls back to another platform",
          "[dreamcast][dc-boss-assets]") {
    test::FakeRenderDevice device;
    const auto absent = test::dataDirectory() / "absent-dreamcast-boss-banks";
    std::vector<std::string> warnings;
    CHECK_FALSE(loadLegendsDreamcastBossAssets(device, absent, absent, 38, {}, warnings));
    CHECK_FALSE(loadLegendsDreamcastBossAssets(device, absent, absent, 39, {}, warnings));
    CHECK_THROWS_AS(loadLegendsDreamcastBossAssets(device, absent, absent, 34, {}, warnings),
                    FileError);
}

TEST_CASE("Dreamcast boss camera reads native attention rather than reward or drift fields",
          "[dreamcast][dc-boss-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    struct Expected {
        const char* bank;
        f32 attention;
    };
    const std::array rows{Expected{"dragon", 0}, Expected{"chimera", 10}, Expected{"djinn", 10},
                          Expected{"drider", 4}, Expected{"skorne1", 24}, Expected{"skorne2", 22}};
    for (const auto& row : rows) {
        CAPTURE(row.bank);
        const auto bytes = readFile(root / "critters" / (std::string(row.bank) + ".wad"));
        const auto camera = legendsDreamcastBossCamera(bytes);
        CHECK(camera.attentionHeight == row.attention);
        CHECK(std::isfinite(camera.floorOffset));
    }
    CHECK_THROWS_AS(legendsDreamcastBossCamera({}), FormatError);
}

TEST_CASE("Dreamcast six native bosses bind all authored attacks and collision nodes",
          "[dreamcast][dc-boss-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    test::FakeRenderDevice device;
    // Deliberately supply no GameCube or N64 bank. Body, moves and effects must
    // remain bindable solely from this expansion's immutable asset directory.
    const auto common = test::dataDirectory() / "absent-common-boss-banks";
    for (const s32 kind : {34, 35, 36, 37, 42, 43}) {
        DYNAMIC_SECTION("Dreamcast boss " << kind) {
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, common, kind, {}, warnings);
            REQUIRE(stock);
            REQUIRE(stock->tree);
            CHECK(stock->body.bound());
            CHECK(stock->tree->name == stock->data.tree());
            CHECK(stock->initialMoveBeforeWake);
            CHECK(stock->wakeDelay == 2);
            CHECK(stock->data.rules().restoreCaptureSupport);
            const auto validate = [&](const CritterData& part) {
                for (const auto& move : part.moves()) {
                    CAPTURE(move.name, move.anim, move.colnode);
                    const auto index = stock->tree->findSequence(move.anim);
                    REQUIRE(index);
                    if (!move.colnode.empty()) {
                        CHECK(stock->tree->findNode(move.colnode, kCombatantNodeNameLength));
                    }
                    TreePose pose;
                    const auto& sequence = stock->tree->sequences[*index];
                    for (const f32 fraction : {0.0f, 0.5f, 1.0f}) {
                        pose.evaluate(*stock->tree, *index,
                                      static_cast<f32>(std::max(sequence.frames - 1, 0)) *
                                          fraction);
                        for (const auto& matrix : pose.matrices()) {
                            for (int col = 0; col < 4; ++col) {
                                for (int row = 0; row < 4; ++row) {
                                    REQUIRE(std::isfinite(matrix[col][row]));
                                }
                            }
                        }
                    }
                }
            };
            validate(stock->data);
            for (const auto& child : stock->children) {
                validate(child);
            }
            CHECK(stock->children.size() == (kind == 35 ? 3 : 0));
            if (kind == 42 || kind == 43) {
                const auto roar = stock->archive.trees.find("BOSSROARFX");
                REQUIRE(roar);
                CHECK(stock->archive.trees.tree(*roar).sequences.front().frames == 18);
            }
        }
    }
}

TEST_CASE("Dreamcast Dragon and Chimera select attacks relative to their bodies",
          "[dreamcast][dc-boss-ai][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    test::FakeRenderDevice device;
    for (const s32 kind : {34, 35}) {
        DYNAMIC_SECTION("kind " << kind) {
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, {}, kind, {}, warnings);
            REQUIRE(stock);
            Combatant actor;
            REQUIRE(actor.spawn(*stock, 0, {0, 0, 0}, 0, nullptr, {}, 'A'));
            // Run the actual native START before testing an independently chosen
            // attack. setMove() intentionally holds a move, so is not an AI test.
            for (s32 frame = 0; frame < 600 && actor.moveType() == MoveDefinition::kStart;
                 ++frame) {
                actor.update(1, 1.0f / 60, {});
            }
            REQUIRE(actor.moveType() != MoveDefinition::kStart);
            Combatant::LocalSnapshot initial;
            REQUIRE(actor.saveLocal(initial));
            const Vec3 start = actor.position();
            for (const f32 distance : {10.0f, 45.0f}) {
                CAPTURE(distance);
                REQUIRE(actor.restoreLocal(initial));
                std::array players{EnemyView{.player = 0,
                                             .position = start + Vec3{0, 0, distance},
                                             .radius = 1.5f,
                                             .height = 5}};
                std::set<std::string> attacks;
                bool closeAttack = false;
                for (s32 frame = 0; frame < 600; ++frame) {
                    actor.update(1, 1.0f / 60, players);
                    const auto inspect = [&](const Combatant& part) {
                        if (part.moveType() < MoveDefinition::kAttackFrom) {
                            return;
                        }
                        attacks.emplace(part.moveName());
                        const auto index = part.data()->moveNamed(part.moveName());
                        REQUIRE(index);
                        const auto& move = part.data()->moves()[*index];
                        if (move.target.maxDistance > 0 && move.target.maxDistance <= 26) {
                            closeAttack = true;
                        }
                    };
                    inspect(actor);
                    for (s32 child = 1; child <= static_cast<s32>(actor.childCount()); ++child) {
                        if (const auto* part = actor.child(child)) {
                            inspect(*part);
                        }
                    }
                    actor.takeCues();
                    actor.takeBlows();
                    actor.takeShots();
                }
                INFO("selected native attacks: " << attacks.size());
                CHECK_FALSE(attacks.empty());
                if (distance == 10) {
                    CHECK(closeAttack);
                } else {
                    CHECK_FALSE(closeAttack);
                }
            }
        }
    }
}

TEST_CASE("Dreamcast melee contact follows native nodes and damaging frame windows",
          "[dreamcast][dc-boss-ai][dc-boss-melee][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    test::FakeRenderDevice device;
    for (const s32 kind : {34, 36, 37, 42, 43}) {
        DYNAMIC_SECTION("kind " << kind) {
            usize checked = 0;
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, {}, kind, {}, warnings);
            REQUIRE(stock);
            Combatant actor;
            REQUIRE(actor.spawn(*stock, 0, {12, 3, -9}, .4f, nullptr, {}, 'A'));
            Combatant::LocalSnapshot initial;
            REQUIRE(actor.saveLocal(initial));
            for (usize index = 0; index < stock->data.moves().size(); ++index) {
                const auto& move = stock->data.moves()[index];
                const auto* damage = stock->data.damage(move.damage0);
                if (!damage || damage->type != AttackDefinition::kBlow || damage->damage <= 0 ||
                    move.frameStart <= 0 || move.colnode.empty()) {
                    continue;
                }
                ++checked;
                CAPTURE(move.name, move.colnode);
                const s32 last = std::max(move.frameStart, move.frameEnd);
                for (const s32 frame : {move.frameStart - 1, move.frameStart, last, last + 1}) {
                    CAPTURE(frame);
                    REQUIRE(actor.restoreLocal(initial));
                    REQUIRE(actor.setMove(
                        index,
                        {.frame = static_cast<f32>(frame), .target = 0, .yaw = std::nullopt}));
                    const auto node = actor.nodeTransform(move.colnode);
                    REQUIRE(node);
                    const Vec3 centre{*node * Vec4{damage->offset, 1}};
                    Combatant::LocalSnapshot atFrame;
                    REQUIRE(actor.saveLocal(atFrame));
                    for (const bool outside : {false, true}) {
                        CAPTURE(outside);
                        REQUIRE(actor.restoreLocal(atFrame));
                        std::array players{EnemyView{
                            .player = 0,
                            .position =
                                centre + Vec3{damage->maxDistance + 1.5f + (outside ? .05f : -.05f),
                                              -2.5f, 0},
                            .radius = .5f,
                            .height = 5,
                            .collisionHeight = 2.5f,
                            .collisionRadius = 1.5f}};
                        actor.update(1, 0, players);
                        const auto blows = actor.takeBlows();
                        const bool active = frame >= move.frameStart && frame <= last;
                        CHECK(blows.empty() == (!active || outside));
                        if (active && !outside) {
                            REQUIRE(blows.size() == 1);
                            CHECK(blows.front().damage == damage->damage);
                            CHECK(glm::distance(blows.front().origin, centre) < .001f);
                        }
                    }
                }
            }
            if (kind == 34) {
                CHECK(checked == 2);
            }
        }
    }
}

TEST_CASE(
    "Dreamcast Djinn Drider and Skorne autonomously keep attacking nearby and distant players",
    "[dreamcast][dc-boss-ai][dc-boss-autonomous][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/djinn.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    struct Case {
        std::string_view level;
        f32 nearDistance;
        f32 farDistance;
    };
    // Stay within the authored melee/projectile bands. These are target probes,
    // not claims that every native move must be selected or share one cooldown.
    constexpr std::array kCases{Case{"levelc5", 15, 55}, Case{"leveld5", 15, 45},
                                Case{"levele2", 26, 55}, Case{"levelf2", 18, 55}};
    test::FakeRenderDevice device;
    for (const auto& testCase : kCases) {
        DYNAMIC_SECTION(testCase.level) {
            const auto encounter = legendsDreamcastBoss(
                readFile(root / formats::dreamcastLegendsWorldDataFile(testCase.level)),
                readFile(root / "levels" / testCase.level / "worlds.rom"), testCase.level);
            REQUIRE(encounter);
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, {}, encounter->combat.kind,
                                                        {}, warnings);
            REQUIRE(stock);
            Combatant actor;
            REQUIRE(actor.spawn(*stock, 0, encounter->combat.position, encounter->combat.yaw,
                                nullptr, {}, 'A'));
            for (s32 tick = 0; tick < 900 && actor.moveType() == MoveDefinition::kStart; ++tick) {
                actor.update(1, 1.f / 60, {});
            }
            REQUIRE(actor.moveType() != MoveDefinition::kStart);
            Combatant::LocalSnapshot afterEntrance;
            REQUIRE(actor.saveLocal(afterEntrance));
            for (const f32 distance : {testCase.nearDistance, testCase.farDistance}) {
                CAPTURE(distance);
                REQUIRE(actor.restoreLocal(afterEntrance));
                const Vec3 direction{std::sin(actor.yaw()), 0, std::cos(actor.yaw())};
                const std::array players{
                    EnemyView{.player = 0,
                              .position = actor.position() + direction * distance,
                              .radius = 1.5f,
                              .height = 5}};
                std::set<u64> attackGenerations;
                std::set<std::string> attacks;
                s32 animatedTicks = 0;
                for (s32 tick = 0; tick < 1800; ++tick) {
                    const f32 frame = actor.animation().frame();
                    const u64 generation = actor.animation().generation();
                    actor.update(1, 1.f / 60, players);
                    REQUIRE(actor.alive());
                    REQUIRE(std::isfinite(actor.position().x));
                    REQUIRE(std::isfinite(actor.position().y));
                    REQUIRE(std::isfinite(actor.position().z));
                    if (frame != actor.animation().frame() ||
                        generation != actor.animation().generation()) {
                        ++animatedTicks;
                    }
                    if (actor.moveType() >= MoveDefinition::kAttackFrom) {
                        attacks.emplace(actor.moveName());
                        attackGenerations.insert(actor.animation().generation());
                    }
                    actor.takeCues();
                    actor.takeBlows();
                    actor.takeGrabs();
                    actor.takePushes();
                    actor.takeSpews();
                    actor.takeShots();
                }
                INFO("autonomous attack names: " << attacks.size()
                                                 << "; entries: " << attackGenerations.size());
                CHECK_FALSE(attacks.empty());
                CHECK(attackGenerations.size() >= 2);
                CHECK(animatedTicks > 60);
            }
        }
    }
}

TEST_CASE("Dreamcast Spider Queen is held inside her territory from her first update asleep",
          "[dreamcast][dc-boss-ai][boss-movement][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "critters/drider.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    const auto encounter =
        legendsDreamcastBoss(readFile(root / formats::dreamcastLegendsWorldDataFile("leveld5")),
                             readFile(root / "levels/leveld5/worlds.rom"), "leveld5");
    REQUIRE(encounter);
    REQUIRE(encounter->combat.kind == boss_type::kDrider);
    // D5's mark and DRIDER's home and roam as the GameCube has them (TYPE +0x94, +0xA0).
    const Vec3 mark = encounter->combat.position;
    CHECK(mark.x == -52.75f);
    CHECK(mark.z == -121.0f);
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsDreamcastBossAssets(device, root, {}, boss_type::kDrider, {}, warnings);
    REQUIRE(stock);
    REQUIRE(stock->data.movement().home.has_value());
    const Vec3 home = *stock->data.movement().home;
    CHECK(home == Vec3{-30, 57, -112});
    CHECK(stock->data.movement().roamRadius == 22);
    Bosses bosses;
    bosses.open(device, {}, nullptr, {}, 'D');
    REQUIRE(bosses.preload(boss_type::kDrider, std::move(stock)));
    REQUIRE(bosses.spawn(boss_type::kDrider, mark, encounter->combat.yaw));
    // The Dreamcast's translate clamps a zero step too (1ST_READ.BIN 0x8c08a5c0, by the fast
    // length at 0x8c0ed4a0), so her first update, asleep in INIT, takes her 2.83 inward.
    const std::array players{EnemyView{.player = 0, .position = Vec3{0, 0, 500}}};
    bosses.update(1, 1.f / 60, players);
    REQUIRE_FALSE(bosses.view().awake);
    CHECK(bosses.position()->x == Approx(-50.1214f));
    CHECK(bosses.position()->z == Approx(-119.9601f));
    CHECK(approximateFlatLength(*bosses.position() - home) == Approx(22));
}
} // namespace
