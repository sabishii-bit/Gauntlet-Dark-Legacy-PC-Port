#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "formats/CritterWad.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantProjectile.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Native Skorne melee uses its animated wrist and authored harmful frames",
          "[legends][bosses][native-boss-melee][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "sko1/moves.wad")) {
        SKIP("native N64 boss extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    for (const s32 kind : {42, 43}) {
        DYNAMIC_SECTION("native boss kind " << kind) {
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto assets =
                loadLegendsBossAssets(device, nativeRoot, root, kind, program, {}, warnings);
            REQUIRE(assets);
            REQUIRE(assets->tree);
            Combatant actor;
            REQUIRE(actor.spawn(*assets, 0, Vec3{12, 3, -9}, 0.4f, nullptr, {}, 'F'));
            Combatant::LocalSnapshot initial;
            REQUIRE(actor.saveLocal(initial));

            // Deliberately set one actual native move on the actor. Its skeleton,
            // tracks, damage and frame windows remain untouched; a random AI choice
            // must not decide whether melee is tested.
            usize tested = 0;
            for (usize index = 0; index < assets->data.moves().size(); ++index) {
                const auto& move = assets->data.moves()[index];
                const auto* damage = assets->data.damage(move.damage0);
                if (damage == nullptr || damage->type != AttackDefinition::kBlow ||
                    damage->damage <= 0 || move.frameStart <= 0 || move.colnode.empty()) {
                    continue;
                }
                CAPTURE(kind, move.name, move.anim, move.colnode, move.frameStart, move.frameEnd);
                CHECK(move.name == "CLAWL");
                const auto node = assets->tree->findNode(move.colnode, kCombatantNodeNameLength);
                REQUIRE(node);
                const auto sequenceIndex = assets->tree->findSequence(move.anim);
                REQUIRE(sequenceIndex);
                const auto& sequence = assets->tree->sequences[*sequenceIndex];
                const s32 last = std::max(move.frameStart, move.frameEnd);
                REQUIRE(sequence.frames > last + 1);
                REQUIRE(damage->maxDistance > 0);
                if (kind == 42 && move.name == "CLAWL") {
                    CHECK(move.anim == "AT1");
                    CHECK(move.colnode == "BODY1_L_WRIST");
                    CHECK(move.frameStart == 6);
                    CHECK(move.frameEnd == 9);
                    CHECK(damage->maxDistance == 10);
                    CHECK(damage->damage == 100);
                }
                if (kind == 43) {
                    // Native BNAM maps the fifteen-character attack reference
                    // RESETBODY1_L_WR to node12, independently of OBHD's suffix.
                    CHECK(*node == 12);
                    CHECK(move.colnode == "BODY1_L_WRIST");
                    CHECK(damage->damage == 150);
                }

                for (const s32 frame : {move.frameStart - 1, move.frameStart, last, last + 1}) {
                    CAPTURE(frame);
                    REQUIRE(actor.restoreLocal(initial));
                    REQUIRE(actor.setMove(index, {.frame = static_cast<f32>(frame), .target = 0}));
                    Combatant::LocalSnapshot state;
                    REQUIRE(actor.saveLocal(state));
                    const auto wrist = actor.nodeTransform(move.colnode);
                    REQUIRE(wrist);
                    const Vec3 centre{*wrist * Vec4{damage->offset, 1}};
                    REQUIRE(std::isfinite(centre.x));
                    REQUIRE(std::isfinite(centre.y));
                    REQUIRE(std::isfinite(centre.z));

                    // Boss.bin e0887154/e0888598 tests the current node point
                    // against the expanded player cylinder. Both segment endpoints
                    // are current; the previous point only supplies knock direction.
                    constexpr f32 kContactRadius = 1.5f;
                    constexpr f32 kHeight = 5.0f;
                    std::array players{EnemyView{.player = 0,
                                                 .position = centre - Vec3{0, 2.5f, 0},
                                                 .radius = 0.5f,
                                                 .height = kHeight,
                                                 .collisionHeight = 2.5f,
                                                 .collisionRadius = kContactRadius}};
                    const bool active = frame >= move.frameStart && frame <= last;
                    for (const bool outside : {false, true}) {
                        CAPTURE(outside);
                        REQUIRE(actor.restoreLocal(state));
                        players[0].position.x = centre.x + damage->maxDistance + kContactRadius +
                                                (outside ? 0.05f : -0.05f);
                        // Zero elapsed time samples the prepared native frame without
                        // moving its wrist or changing the selected attack.
                        actor.update(1, 0, players);
                        CHECK(actor.moveName() == move.name);
                        const auto blows = actor.takeBlows();
                        CHECK(blows.empty() == (!active || outside));
                        if (active && !outside) {
                            REQUIRE(blows.size() == 1);
                            CHECK(blows.front().player == 0);
                            CHECK(blows.front().damage == Approx(damage->damage));
                            CHECK(blows.front().gated);
                            CHECK(glm::distance(blows.front().origin, centre) < 0.001f);
                        }
                    }
                    REQUIRE(actor.restoreLocal(state));
                    players[0].position = centre - Vec3{0, 2.5f, 0};
                    players[0].damageable = false;
                    actor.update(1, 0, players);
                    CHECK(actor.takeBlows().empty());
                }
                ++tested;
            }
            // Both native banks contain one type0 blow. GRAB is type7 and
            // the ground-pound/wing attacks are type3, not additional claws.
            CHECK(tested == 1);
        }
    }
}
} // namespace

TEST_CASE("Native Skorne wing gust reaches standing players throughout its authored area",
          "[legends][native-skorn-contact][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "sko1/moves.wad"))
        SKIP("native N64 boss extraction is not installed");
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    test::FakeRenderDevice device;
    for (const s32 kind : {42, 43}) {
        std::vector<std::string> warnings;
        auto assets = loadLegendsBossAssets(device, nativeRoot, root, kind, program, {}, warnings);
        REQUIRE(assets);
        Combatant actor;
        REQUIRE(actor.spawn(*assets, 0, Vec3{0}, 0, nullptr, {}, 'F'));
        const auto moveAt =
            std::ranges::find(assets->data.moves(), "WING_ATTACK", &MoveDefinition::name);
        REQUIRE(moveAt != assets->data.moves().end());
        const auto sequence = assets->tree->findSequence(moveAt->anim);
        REQUIRE(sequence);
        REQUIRE(actor.setMove(static_cast<usize>(moveAt - assets->data.moves().begin()),
                              {.frame = 0.0f, .target = 0}));
        const std::array players{EnemyView{.player = 0,
                                           .position = {0, 0, 25},
                                           .radius = 1.5f,
                                           .height = 5,
                                           .collisionHeight = 2.5f,
                                           .collisionRadius = 1.5f}};
        bool hit = false;
        for (s32 tick = 0; tick < 120; ++tick) {
            actor.update(1, 1.0f / 60, players);
            for (const auto& blow : actor.takeBlows()) {
                hit |= blow.damage > 0 && (blow.flags & 0x20U) != 0;
            }
        }
        CAPTURE(kind);
        CHECK(hit);
    }
}

TEST_CASE("Native expanding attacks retain damage and reach for their entire growth",
          "[legends][native-skorn-contact]") {
    // game.bin e0078270: radius=maximum*elapsedPercent/100; Boss.bin
    // e0881e34 keeps the authored damage, unlike GDL's declining wave.
    CritterArea area;
    area.radius = 35;
    area.damage = 80;
    area.expanding = area.linearGrowth = true;
    area.halfHeight = 50;
    area.lifetime = 1;
    const EnemyView player{.player = 0, .position = {0, 0, 30}, .radius = 1.5f, .height = 5};
    const Mat4 elevated = glm::translate(Mat4{1}, Vec3{0, 20, 0});
    area.secondsLeft = 1;
    CHECK(area.currentRadius() == 0);
    area.secondsLeft = .5f;
    CHECK(area.currentRadius() == Approx(17.5f));
    CHECK(area.currentDamage() == 80);
    CHECK_FALSE(area.touches(elevated, player));
    area.secondsLeft = .1f;
    CHECK(area.currentRadius() == Approx(31.5f));
    CHECK(area.currentDamage() == 80);
    CHECK(area.touches(elevated, player));
    auto behind = player;
    behind.position.z = -30;
    CHECK(area.touches(elevated, behind));
    area.secondsLeft = 0;
    CHECK_FALSE(area.touches(elevated, player));
}

TEST_CASE("Native projectile rate interpolation reaches the whole authored speed interval",
          "[legends][native-skorn-contact]") {
    formats::CritterFile source;
    source.descriptors.push_back({});
    source.types.push_back({});
    source.types[0].descriptorIndex = 0;
    source.types[0].moveIndex = 0;
    source.types[0].moveCount = 1;
    source.moves.push_back({});
    const CritterRules rules{
        .projectileMaxRate = 2.5f, .projectileRateScale = .5f, .ballisticTargetOffset = -4};
    const auto native = CritterData::fromDecoded("SKORNE1", source, 0, rules);
    AttackDefinition damage;
    damage.speed = 20;
    damage.maxSpeed = 60;
    damage.behaviorFlags = CombatantProjectile::kAimAtPlayer;
    damage.gravity = 5;
    CombatShot shot;
    shot.data = &native;
    shot.origin = {0, 20, 0};
    shot.target = Vec3{0, 2.5f, 30};
    for (const f32 rate : {.5f, 1.0f, 1.5f, 2.0f, 2.5f, 5.0f}) {
        shot.rate = rate;
        const f32 expected = 20 + 40 * .5f * (std::clamp(rate, .5f, 2.5f) - .5f);
        const auto velocity = CombatantProjectile::velocity(damage, shot);
        CHECK(velocity.z == Approx(expected));
        CHECK(velocity.y == Approx((-17.5f - 4) / 30 * expected + 2.5f * 30 / expected));
    }
    // The same shared helper with ordinary data still uses the GDL curve.
    shot.data = nullptr;
    shot.rate = 1;
    CHECK(CombatantProjectile::velocity(damage, shot).z == Approx(35));
}
