#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"

#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CritterData.h"

namespace gdl::test {
/** Where a fighter without a home of its own stands to land its blows. */
inline constexpr Vec3 kBlowSpawn{12, 3, -9};
inline constexpr f32 kBlowSpawnYaw = 0.4f;

/** A blow's push as the original's CritterNodePlayerCollide (0x80036424) builds it: twice the
 * sum of the striking node's travel over the native frame and the way from the body's origin to
 * the feet, that given a unit's height, each scaled by the reciprocal of its fast length. */
inline Vec3 swingPush(const Vec3& before, const Vec3& after, const Vec3& body, const Vec3& feet) {
    return 2.0f * (approximateUnit(after - before) +
                   approximateUnit(Vec3{feet.x - body.x, 1.0f, feet.z - body.z}));
}

/** The body's origin a blow pushes away from: the model's root with the TYPE origin turned with
 * it (the critter's +0x4C). */
inline Vec3 bodyOrigin(const game::Combatant& body) {
    const Vec3 root = body.position() + Vec3{0, body.data()->floorOffset(), 0};
    const Mat4 model =
        glm::scale(portable::rotate(glm::translate(Mat4{1}, root), body.yaw(), Vec3{0, 1, 0}),
                   Vec3{body.scale() * body.shrink()});
    return Vec3{model * Vec4{body.data()->originOffset(), 1}};
}

/** The move's type-0 blow, or null when it has none that harms or no node to strike with. */
inline const game::AttackDefinition* blowOf(const game::CritterData& data,
                                            const game::MoveDefinition& move) {
    const game::AttackDefinition* damage = data.damage(move.damage0);
    return damage != nullptr && damage->type == game::AttackDefinition::kBlow &&
                   damage->damage > 0 && !move.colnode.empty()
               ? damage
               : nullptr;
}

/** Spawns `actor` where its territory's clamp never moves it: at its home, or at kBlowSpawn
 * when it has none. */
inline void spawnAtHome(game::Combatant& actor, game::CombatantAssets& assets, char realm) {
    const Vec3 lift{0, assets.data.floorOffset(), 0};
    const Vec3 home = assets.data.movement().home.value_or(kBlowSpawn + lift) - lift;
    REQUIRE(actor.spawn(assets, 0, home, kBlowSpawnYaw, nullptr, {}, realm));
}

/** A player standing half `reach` beside `centre`, across `swing`, so the swing and the way
 * from the body point different ways; its contact centre stands level with `centre`. */
inline std::array<game::EnemyView, 1> besideSwing(const Vec3& centre, const Vec3& swing,
                                                  const Vec3& body, f32 reach) {
    constexpr f32 kStill = 1e-3f;
    constexpr f32 kContactHeight = 2.5f;
    Vec2 along{swing.x, swing.z};
    if (glm::length(along) < kStill) {
        along = Vec2{centre.x - body.x, centre.z - body.z};
    }
    along = glm::length(along) < kStill ? Vec2{1, 0} : glm::normalize(along);
    const Vec2 across = 0.5f * reach * Vec2{-along.y, along.x};
    return {game::EnemyView{
        .player = 0,
        .position = {centre.x + across.x, centre.y - kContactHeight, centre.z + across.y},
        .radius = 0.5f,
        .height = 5.0f,
        .collisionHeight = kContactHeight,
        .collisionRadius = 1.5f}};
}

/** A blow a move landed, with what its push was built from. */
struct LandedBlow {
    std::vector<game::CombatBlow> blows; ///< what the landing update struck
    Vec3 before{0};                      ///< the striking node as the update before noted it
    Vec3 node{0};                        ///< the striking node as the landing update noted it
    Vec3 body{0};                        ///< the body's origin as the landing update began
    Vec3 feet{0};                        ///< where the player struck stood
};

/** Lands the blow of move `index` of `part` (0 for the body, else a head's id) on a player
 * standing half its reach across the striking node's swing, from `actor` standing at rest:
 * the move is set a frame before it strikes, an update of no time notes the node, then native
 * frames are stepped until the blow lands. A body steps and turns after its strikes, so each
 * update's noted node is taken from where the body stood as that update began. A head is
 * driven by itself, since setting the body's move reaches only the body's own moves. `actor`
 * is left as it was found. */
inline LandedBlow landBlow(game::Combatant& actor, s32 part, usize index) {
    constexpr f32 kNativeFrame = 1.0f / 30;
    constexpr s32 kTicksPerFrame = 2;
    constexpr s32 kMostFrames = 120;
    const auto fighter = [&]() -> game::Combatant& {
        if (part == 0) {
            return actor;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto* head = const_cast<game::Combatant*>(actor.child(part));
        REQUIRE(head);
        return *head;
    };
    const game::MoveDefinition& move = fighter().data()->moves()[index];
    const game::AttackDefinition* damage = blowOf(*fighter().data(), move);
    REQUIRE(damage);
    game::Combatant::LocalSnapshot spawned;
    REQUIRE(actor.saveLocal(spawned));
    // Where the move's setup notes the node at `frame` from the body as `state` left it: after
    // the animation, before the step and the turn. A head's own update stands it on the body.
    const auto noted = [&](const game::Combatant::LocalSnapshot& state, f32 frame) {
        REQUIRE(actor.restoreLocal(state));
        REQUIRE(fighter().setMove(index, {.frame = frame, .target = 0, .yaw = std::nullopt}));
        if (part != 0) {
            fighter().update(1, 0.0f, {});
        }
        const auto node = fighter().nodeTransform(move.colnode);
        REQUIRE(node);
        return *node;
    };
    const auto opening = static_cast<f32>(std::max(move.frameStart, 0));
    const Mat4 strikes = noted(spawned, opening);
    const Vec3 swing = Vec3{noted(spawned, opening + 1)[3]} - Vec3{strikes[3]};
    REQUIRE(actor.restoreLocal(spawned));
    const auto players = besideSwing(Vec3{strikes * Vec4{damage->offset, 1}}, swing,
                                     bodyOrigin(actor), damage->maxDistance);

    struct Update {
        std::vector<game::CombatBlow> blows;
        Vec3 node{0};
        Vec3 body{0};
    };
    const auto advance = [&](s32 ticks, f32 seconds, std::span<const game::EnemyView> roster) {
        game::Combatant::LocalSnapshot before;
        REQUIRE(actor.saveLocal(before));
        Update out;
        out.body = bodyOrigin(actor);
        fighter().update(ticks, seconds, roster);
        out.blows = fighter().takeBlows();
        const f32 frame = fighter().animation().frame();
        game::Combatant::LocalSnapshot after;
        REQUIRE(actor.saveLocal(after));
        out.node = Vec3{noted(before, frame)[3]};
        REQUIRE(actor.restoreLocal(after));
        return out;
    };
    REQUIRE(fighter().setMove(index, {.frame = static_cast<f32>(std::max(move.frameStart - 1, 0)),
                                      .target = 0,
                                      .yaw = std::nullopt}));
    // An update of no time notes the node without moving it: the sample the first native frame
    // measures from.
    const Update primed = advance(1, 0.0f, {});
    REQUIRE(primed.blows.empty());
    LandedBlow landed;
    landed.before = primed.node;
    landed.feet = players[0].position;
    std::optional<Update> landing;
    const auto last = static_cast<f32>(std::max(move.frameStart, move.frameEnd));
    for (s32 frame = 0;
         frame < kMostFrames && !landing && std::floor(fighter().animation().frame()) <= last;
         ++frame) {
        Update step = advance(kTicksPerFrame, kNativeFrame, players);
        if (step.blows.empty()) {
            landed.before = step.node;
        } else {
            landing = std::move(step);
        }
    }
    REQUIRE(landing);
    CHECK(std::floor(fighter().animation().frame()) >= static_cast<f32>(move.frameStart));
    REQUIRE(landing->blows.size() == 1);
    CHECK(landing->blows[0].player == 0);
    landed.blows = std::move(landing->blows);
    landed.node = landing->node;
    landed.body = landing->body;
    REQUIRE(actor.restoreLocal(spawned));
    return landed;
}
} // namespace gdl::test
