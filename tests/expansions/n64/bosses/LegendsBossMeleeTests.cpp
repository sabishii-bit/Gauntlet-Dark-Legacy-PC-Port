#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "formats/CritterWad.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantProjectile.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kNativeFrame = 1.0f / 30; ///< the original's frame, over which a blow's swing runs
constexpr s32 kTicksPerFrame = 2;
constexpr s32 kMostFrames = 120;
constexpr f32 kStill = 1e-3f;
constexpr f32 kTolerance = 1e-3f;

/** A blow's push as Boss.bin's CritterNodePlayerCollide (e0887154) builds it: twice the sum of
 * the node's travel over the frame and the way from the body's origin to the feet, that given a
 * unit's height, each scaled by the reciprocal of its fast length (game.bin e00145c4). */
Vec3 swingPush(const Vec3& before, const Vec3& after, const Vec3& body, const Vec3& feet) {
    return 2.0f * (approximateUnit(after - before) +
                   approximateUnit(Vec3{feet.x - body.x, 1.0f, feet.z - body.z}));
}

/** `v` laid flat at a unit's length, or nought when it has no flat length. */
Vec3 flatUnit(const Vec3& v) {
    const f32 length = std::hypot(v.x, v.z);
    return length > 0 ? Vec3{v.x / length, 0, v.z / length} : Vec3{0};
}

/** The body's origin a blow pushes away from: the model's root with the TYPE origin turned with
 * it (c+0x4C, ProcessCritter e0884a9c). */
Vec3 bodyOrigin(const Combatant& body) {
    const Vec3 root = body.position() + Vec3{0, body.data()->floorOffset(), 0};
    const Mat4 model =
        glm::scale(portable::rotate(glm::translate(Mat4{1}, root), body.yaw(), Vec3{0, 1, 0}),
                   Vec3{body.scale() * body.shrink()});
    return Vec3{model * Vec4{body.data()->originOffset(), 1}};
}

/** Where move `index`'s node stands at `frame` from the body as `state` left it: what MoveSetup
 * (e08839bc) notes each frame, after the animation and before the step and the turn. */
Mat4 notedNode(Combatant& actor, const Combatant::LocalSnapshot& state, usize index, f32 frame) {
    REQUIRE(actor.restoreLocal(state));
    REQUIRE(actor.setMove(index, {.frame = frame, .target = 0}));
    const auto node = actor.nodeTransform(actor.data()->moves()[index].colnode);
    REQUIRE(node);
    return *node;
}

/** What an update did: its blows, and where the move's setup noted the node and stood the body's
 * origin. */
struct Noted {
    std::vector<CombatBlow> blows;
    Vec3 node{0};
    Vec3 body{0};
};

/** Lands move `index`'s blow from a body spawned at home on a player standing half its reach
 * across the node's swing, and checks that the blow keeps only the flat way of the push Boss.bin
 * builds, at a unit's length: e0886ed0 makes the push a point and game.bin e004ba94 turns the
 * player to it, taking nothing else. */
void checkSwingPush(CombatantAssets& assets, usize index) {
    const MoveDefinition& move = assets.data.moves()[index];
    const AttackDefinition* damage = assets.data.damage(move.damage0);
    REQUIRE(damage);
    // Standing at home, the territory's clamp never moves it.
    const f32 floorOffset = assets.data.floorOffset();
    const Vec3 home = assets.data.movement().home.value_or(Vec3{12, 3 + floorOffset, -9}) -
                      Vec3{0, floorOffset, 0};
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, home, 0.4f, nullptr, {}, 'F'));
    Combatant::LocalSnapshot spawned;
    REQUIRE(actor.saveLocal(spawned));
    const auto opening = static_cast<f32>(move.frameStart);
    const Mat4 strikes = notedNode(actor, spawned, index, opening);
    const Vec3 centre{strikes * Vec4{damage->offset, 1}};
    const Vec3 swing = Vec3{notedNode(actor, spawned, index, opening + 1)[3]} - Vec3{strikes[3]};
    REQUIRE(std::hypot(swing.x, swing.z) > kStill);
    const Vec2 along = glm::normalize(Vec2{swing.x, swing.z});
    REQUIRE(actor.restoreLocal(spawned));
    const Vec2 across = 0.5f * damage->maxDistance * Vec2{-along.y, along.x};
    constexpr f32 kContactHeight = 2.5f;
    const std::array players{
        EnemyView{.player = 0,
                  .position = {centre.x + across.x, centre.y - kContactHeight, centre.z + across.y},
                  .radius = 0.5f,
                  .height = 5,
                  .collisionHeight = kContactHeight,
                  .collisionRadius = 1.5f}};

    // The body turns after its strikes, so each update's noted node is taken from where it
    // stood as the update began.
    const auto advance = [&](s32 ticks, f32 seconds, std::span<const EnemyView> roster) {
        Combatant::LocalSnapshot before;
        REQUIRE(actor.saveLocal(before));
        Noted out;
        out.body = bodyOrigin(actor);
        actor.update(ticks, seconds, roster);
        out.blows = actor.takeBlows();
        const f32 frame = actor.animation().frame();
        Combatant::LocalSnapshot after;
        REQUIRE(actor.saveLocal(after));
        out.node = Vec3{notedNode(actor, before, index, frame)[3]};
        REQUIRE(actor.restoreLocal(after));
        return out;
    };
    REQUIRE(actor.setMove(index, {.frame = opening - 1, .target = 0}));
    // An update of no time notes the node without moving it: the sample the first frame
    // measures from.
    const Noted primed = advance(1, 0.0f, {});
    REQUIRE(primed.blows.empty());
    Vec3 nodeBefore = primed.node;
    std::optional<Noted> landing;
    const auto last = static_cast<f32>(std::max(move.frameStart, move.frameEnd));
    for (s32 frame = 0;
         frame < kMostFrames && !landing && std::floor(actor.animation().frame()) <= last;
         ++frame) {
        Noted step = advance(kTicksPerFrame, kNativeFrame, players);
        if (step.blows.empty()) {
            nodeBefore = step.node;
        } else {
            landing = std::move(step);
        }
    }
    REQUIRE(landing);
    REQUIRE(landing->blows.size() == 1);
    CHECK(landing->blows[0].player == 0);
    const Vec3 expected =
        flatUnit(swingPush(nodeBefore, landing->node, landing->body, players[0].position));
    // The case tells the swing's way from the way straight out from the body.
    REQUIRE(glm::distance(expected, flatUnit(players[0].position - landing->body)) > 0.01f);
    const Vec3& direction = landing->blows[0].direction;
    CAPTURE(direction.x, direction.y, direction.z, expected.x, expected.z);
    CHECK(direction.y == 0);
    CHECK(glm::length(direction) == Approx(1));
    CHECK(glm::distance(direction, expected) < kTolerance);
}

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
                checkSwingPush(*assets, index);
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
    if (!std::filesystem::is_regular_file(nativeRoot / "sko1/moves.wad")) {
        SKIP("native N64 boss extraction is not installed");
    }
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
