#include <array>
#include <cmath>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/CombatantFixture.h"
#include "game/enemies/CombatantGaze.h"
#include "game/enemies/HeadedBodyFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kFrame = 1.0f / 30.0f;

/** A body with a neck and a head on it, and one still sequence. */
TreeInfo neckedTree() {
    TreeInfo tree;
    tree.name = "BODY";
    TreeNodeInfo body;
    body.name = "BODY";
    TreeNodeInfo neck;
    neck.name = "NECK";
    neck.parent = 0;
    neck.position = Vec3{0.0f, 5.0f, 0.0f};
    TreeNodeInfo head;
    head.name = "HEAD";
    head.parent = 1;
    head.position = Vec3{0.0f, 1.0f, 0.0f};
    tree.nodes = {body, neck, head};
    TreeSequenceInfo sequence;
    sequence.name = "READY";
    sequence.frames = 3;
    sequence.frameRate = 30;
    sequence.trackOfNode = {-1, -1, -1};
    tree.sequences.push_back(sequence);
    return tree;
}

/** The way a node's z axis points, about the upright. */
f32 yawOf(const Mat4& matrix) {
    return std::atan2(matrix[2].x, matrix[2].z);
}

TEST_CASE("a look node chases its target a quarter turn a second, no further from the "
          "animation than its limit, and comes back without one",
          "[game][combatant][gaze]") {
    const TreeInfo tree = neckedTree();
    std::array<LookDefinition, 1> looks;
    looks[0].node = "HEAD";
    looks[0].yawRate = 0.3f;
    looks[0].pitchRate = 0.2f;
    CombatantGaze gaze;
    TreePose pose;
    // The animation places the head afresh every frame; the gaze turns it on top.
    const auto frame = [&](const std::optional<Vec3>& target) {
        pose.evaluate(tree, 0, 0.0f);
        gaze.aim(pose, tree, Mat4{1.0f}, looks, target, kFrame);
    };
    const Vec3 beside{100.0f, 6.0f, 0.0f}; // level with the head, square to its right
    frame(beside);
    REQUIRE(gaze.tracked(0).x == Approx(kHalfPi * kFrame));
    REQUIRE(gaze.tracked(0).y == Approx(0.0f).margin(1e-5f));
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(kHalfPi * kFrame));
    for (s32 i = 0; i < 40; ++i) {
        frame(beside);
    }
    // The bearing reaches the target; the head, placed afresh by the animation every frame,
    // is turned from it by no more than its limit.
    REQUIRE(gaze.tracked(0).x == Approx(kHalfPi));
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.3f));
    // Without a target the bearing returns to nought at the same pace, the head with it once
    // the bearing comes within its limit.
    frame(std::nullopt);
    REQUIRE(gaze.tracked(0).x == Approx(kHalfPi - kHalfPi * kFrame));
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.3f));
    for (s32 i = 0; i < 40; ++i) {
        frame(std::nullopt);
    }
    REQUIRE(gaze.tracked(0).x == Approx(0.0f).margin(1e-5f));
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.0f).margin(1e-5f));

    // A head allowed a wider turn than the bearing follows it exactly.
    looks[0].yawRate = 2.0f;
    gaze.reset();
    frame(beside);
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(kHalfPi * kFrame));
    for (s32 i = 0; i < 40; ++i) {
        frame(beside);
    }
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(kHalfPi));
    REQUIRE(Vec3{pose.matrices()[2][2]}.x == Approx(1.0f));
    // A tight one lags it from the first frame.
    looks[0].yawRate = 0.01f;
    gaze.reset();
    frame(beside);
    REQUIRE(gaze.tracked(0).x == Approx(kHalfPi * kFrame));
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.01f));

    // The pitch bias tips where it looks; a target straight ahead is looked at by the bias.
    looks[0].yawRate = 0.3f;
    looks[0].pitchBias = 0.1f;
    gaze.reset();
    for (s32 i = 0; i < 40; ++i) {
        frame(Vec3{0.0f, 6.0f, 100.0f});
    }
    REQUIRE(gaze.tracked(0).y == Approx(0.1f));
    REQUIRE(pose.readAngles(2).x == Approx(0.1f));
    REQUIRE(pose.readAngles(2).y == Approx(0.0f).margin(1e-5f));

    // A head flagged to turn through its parent turns the neck and leaves the head be.
    looks[0].parent = true;
    looks[0].pitchBias = 0.0f;
    gaze.reset();
    for (s32 i = 0; i < 40; ++i) {
        frame(beside);
    }
    REQUIRE(pose.readAngles(1).y == Approx(0.3f).margin(1e-4f));
    REQUIRE(pose.readAngles(2).y == Approx(0.0f).margin(1e-5f));
    // A rate of nought turns nothing, and a node the tree lacks is left alone.
    looks[0].parent = false;
    looks[0].yawRate = 0.0f;
    looks[0].pitchRate = 0.0f;
    gaze.reset();
    frame(beside);
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.0f).margin(1e-6f));
    looks[0].yawRate = 0.3f;
    looks[0].node = "TAIL";
    frame(beside);
    REQUIRE(yawOf(pose.matrices()[2]) == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("a fighter's head follows its target through its moves: not through the entrance, "
          "back to the animation while a move holds it, frozen, or dying; a head without a "
          "target of its own takes the body's",
          "[game][combatant][gaze][chimera]") {
    test::FakeRenderDevice renderer;
    test::CombatantFixture fight;
    fight.open(renderer, test::headedBodyAssets(), nullptr, {}, 'A');
    REQUIRE(fight.spawn("CHIMERA", Vec3{0.0f}, 0.0f));
    Combatant& body = fight.actor;
    REQUIRE(body.data()->looks()[0].node == "HEAD");
    REQUIRE(body.child(1)->data()->looks()[0].node == "HEAD_L_TIP");
    std::array<EnemyView, 1> players;
    players[0].player = 0;
    players[0].position = Vec3{40.0f, 0.0f, 0.0f};
    players[0].height = 6.0f;
    const auto headYaw = [&](const char* node) { return yawOf(*body.nodeTransform(node)); };
    // Through its entrance the head keeps to the animation.
    fight.update(2, kFrame, players);
    REQUIRE(body.moveType() == MoveDefinition::kStart);
    REQUIRE(headYaw("HEAD") == Approx(0.0f).margin(1e-5f));
    for (s32 i = 0; i < 60; ++i) {
        fight.update(2, kFrame, players);
        body.takeCues();
    }
    REQUIRE(body.moveType() != MoveDefinition::kStart);
    // Then it comes round toward the player square to its right, as far as its limit, and
    // so do the heads, which have no target of their own while they copy the body.
    // (The pitch toward the player, level with its feet, foreshortens the yaw a little.)
    REQUIRE(headYaw("HEAD") == Approx(0.3f).margin(0.01f));
    REQUIRE(headYaw("HEAD_L_TIP") == Approx(0.5f).margin(0.01f));
    REQUIRE(headYaw("HEAD_R_TIP") == Approx(0.5f).margin(0.01f));
    // Held to its stance, the heads copy the body and still turn their tips.
    body.hold(true);
    for (s32 i = 0; i < 60; ++i) {
        fight.update(2, kFrame, players);
        body.takeCues();
    }
    REQUIRE(body.moveType() == MoveDefinition::kReady);
    REQUIRE(body.child(1)->moveType() == -1);
    REQUIRE(headYaw("HEAD_L_TIP") == Approx(0.5f).margin(0.01f));
    body.hold(false);
    // Frozen, it looks nowhere in particular again.
    body.freeze(600);
    for (s32 i = 0; i < 60; ++i) {
        fight.update(2, kFrame, players);
    }
    REQUIRE(headYaw("HEAD") == Approx(0.0f).margin(1e-3f));
    body.freeze(0);
    for (s32 i = 0; i < 60; ++i) {
        fight.update(2, kFrame, players);
        body.takeCues();
    }
    REQUIRE(headYaw("HEAD") == Approx(0.3f).margin(0.01f));
    // A move flagged to hold the head (the body's stare, within five) brings it back.
    players[0].position = Vec3{3.0f, 0.0f, 0.0f};
    bool stared = false;
    for (s32 i = 0; i < 90 && !stared; ++i) {
        fight.update(2, kFrame, players);
        body.takeCues();
        stared = body.moveName() == "STARE";
    }
    REQUIRE(stared);
    for (s32 i = 0; i < 30 && body.moveName() == "STARE"; ++i) {
        fight.update(2, kFrame, players);
    }
    REQUIRE(headYaw("HEAD") < 0.2f);
    // Dying, it looks nowhere.
    EnemyHit hit;
    hit.damage = 10000.0f;
    body.hurt(hit);
    for (s32 i = 0; i < 30 && body.present(); ++i) {
        fight.update(2, kFrame, players);
    }
    REQUIRE(body.dying());
    REQUIRE(headYaw("HEAD") == Approx(0.0f).margin(1e-3f));
}
} // namespace
