#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/world/TreePose.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** A track keying one channel with the given values at the given frames. */
TrackInfo track(u32 node, u16 channel, std::vector<u16> frames, std::vector<f32> values,
                bool pitchYawRoll = false) {
    TrackInfo t;
    t.node = node;
    t.flags = static_cast<u16>(TrackInfo::channelBit(channel) |
                               (pitchYawRoll ? TrackInfo::kPitchYawRoll : 0));
    t.frames = std::move(frames);
    t.values = std::move(values);
    return t;
}

/** A root at (1, 0, 0) with a child two units above it and one sequence of twelve frames. */
TreeInfo sampleTree() {
    TreeInfo tree;
    tree.name = "SAMPLE";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.position = Vec3{1.0f, 0.0f, 0.0f};
    TreeNodeInfo child;
    child.name = "CHILD";
    child.parent = 0;
    child.position = Vec3{0.0f, 2.0f, 0.0f};
    tree.nodes = {root, child};
    TreeSequenceInfo sequence;
    sequence.name = "MOVE";
    sequence.frames = 12;
    sequence.frameRate = 30;
    sequence.tracks.push_back(track(0, 3, {0, 4}, {0.0f, 2.0f})); // root slides +x
    sequence.trackOfNode = {0, -1};
    tree.sequences.push_back(sequence);
    return tree;
}

bool near(const Vec3& a, const Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, 1e-5f));
}

bool near(const Mat4& a, const Mat4& b) {
    for (s32 c = 0; c < 4; ++c) {
        if (!glm::all(glm::epsilonEqual(a[c], b[c], 1e-5f))) {
            return false;
        }
    }
    return true;
}

TEST_CASE("a track samples between keys, holds across large angle steps and past its end",
          "[world][animation][pose]") {
    const TrackInfo pitch = track(0, 0, {0, 4, 11}, {0.0f, 1.0f, 3.5f});
    REQUIRE(pitch.channelCount() == 1);
    REQUIRE(pitch.has(0));
    REQUIRE_FALSE(pitch.has(1));
    REQUIRE(TreePose::sample(pitch, 0.0f).rotation.x == 0.0f);
    REQUIRE(TreePose::sample(pitch, 2.0f).rotation.x == Approx(0.5f));
    REQUIRE(TreePose::sample(pitch, 4.0f).rotation.x == 1.0f);
    // From 1 to 3.5 is more than a right angle, so the earlier key holds until the later one.
    REQUIRE(TreePose::sample(pitch, 8.0f).rotation.x == 1.0f);
    REQUIRE(TreePose::sample(pitch, 11.0f).rotation.x == Approx(3.5f - kTwoPi)); // wrapped
    REQUIRE(TreePose::sample(pitch, 20.0f).rotation.x == Approx(3.5f - kTwoPi));
    // Within an eighth of a frame of a key that key shows as it is.
    REQUIRE(TreePose::sample(pitch, 3.9f).rotation.x == 1.0f);
    // Positions interpolate whatever the step; unkeyed channels rest.
    const TrackInfo slide = track(0, 5, {0, 10}, {0.0f, 10.0f});
    const NodePose mid = TreePose::sample(slide, 5.0f);
    REQUIRE(mid.position.z == Approx(5.0f));
    REQUIRE(mid.position.x == 0.0f);
    REQUIRE(mid.scale == Vec3{1.0f, 1.0f, 1.0f});
    REQUIRE(TreePose::wrapAngle(-kPi) == Approx(kPi));
    REQUIRE(TreePose::wrapAngle(1.0f) == 1.0f);
}

TEST_CASE("subtree overlays keep body motion and unrelated sibling poses",
          "[world][animation][pose]") {
    TreeInfo tree = sampleTree();
    TreeNodeInfo sibling;
    sibling.parent = 0;
    sibling.position = Vec3{0, 0, 5};
    tree.nodes.push_back(sibling);
    tree.sequences[0].tracks.push_back(track(1, 4, {0, 4}, {0, 3}));
    tree.sequences[0].trackOfNode = {0, 1, -1};
    TreePose body;
    body.evaluate(tree, 0, 0);
    TreePose head;
    head.evaluate(tree, 0, 4);
    const Mat4 siblingBefore = body.matrices()[2];
    body.overlaySubtree(head, 1);
    REQUIRE(near(Vec3{body.matrices()[0][3]}, Vec3{1, 0, 0}));
    REQUIRE(near(Vec3{body.matrices()[1][3]}, Vec3{1, 5, 0}));
    REQUIRE(near(body.matrices()[2], siblingBefore));
}

TEST_CASE("a node's matrix turns in the original's sense and carries its rest offset",
          "[world][animation][pose]") {
    NodePose pose;
    pose.rotation.y = 0.7f;
    const Mat4 yawed = TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(near(yawed, glm::rotate(Mat4{1.0f}, 0.7f, Vec3{0.0f, 1.0f, 0.0f})));
    pose.rotation = Vec3{0.4f, 0.0f, 0.0f};
    const Mat4 pitched = TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(near(pitched, glm::rotate(Mat4{1.0f}, -0.4f, Vec3{1.0f, 0.0f, 0.0f})));
    pose.pitchYawRoll = true;
    REQUIRE(near(TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f}), pitched));
    // Both orders agree on a single axis; they differ once two combine.
    pose.rotation = Vec3{0.4f, 0.7f, 0.0f};
    const Mat4 pyr = TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f});
    pose.pitchYawRoll = false;
    const Mat4 ryp = TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE_FALSE(near(pyr, ryp));
    REQUIRE(near(pyr, glm::rotate(Mat4{1.0f}, -0.4f, Vec3{1.0f, 0.0f, 0.0f}) *
                          glm::rotate(Mat4{1.0f}, 0.7f, Vec3{0.0f, 1.0f, 0.0f})));
    REQUIRE(near(ryp, glm::rotate(Mat4{1.0f}, 0.7f, Vec3{0.0f, 1.0f, 0.0f}) *
                          glm::rotate(Mat4{1.0f}, -0.4f, Vec3{1.0f, 0.0f, 0.0f})));

    pose = NodePose{};
    pose.position = Vec3{1.0f, 2.0f, 3.0f};
    pose.scale = Vec3{2.0f, 1.0f, 1.0f};
    const Mat4 placed = TreePose::localMatrix(pose, Vec3{10.0f, 0.0f, 0.0f});
    REQUIRE(near(Vec3{placed[3]}, Vec3{11.0f, 2.0f, 3.0f}));
    REQUIRE(near(Vec3{placed[0]}, Vec3{2.0f, 0.0f, 0.0f}));
    REQUIRE(near(Vec3{placed[1]}, Vec3{0.0f, 1.0f, 0.0f}));
}

TEST_CASE("a tree pose composes each node with its ancestors, mirrors and blends",
          "[world][animation][pose]") {
    const TreeInfo tree = sampleTree();
    TreePose pose;
    REQUIRE_FALSE(pose.posed());
    pose.rest(tree);
    REQUIRE(pose.posed());
    REQUIRE(pose.size() == 2);
    REQUIRE(near(Vec3{pose.matrices()[1][3]}, Vec3{1.0f, 2.0f, 0.0f}));

    pose.evaluate(tree, 0, 2.0f);
    REQUIRE(pose.poses()[0].position.x == Approx(1.0f));
    REQUIRE(near(Vec3{pose.matrices()[0][3]}, Vec3{2.0f, 0.0f, 0.0f}));
    // The child keys nothing and rides along on its parent.
    REQUIRE(pose.poses()[1].position == Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(near(Vec3{pose.matrices()[1][3]}, Vec3{2.0f, 2.0f, 0.0f}));

    pose.evaluate(tree, 0, 4.0f, true);
    REQUIRE(pose.poses()[0].position.x == Approx(-2.0f));
    REQUIRE(pose.poses()[0].scale.x == -1.0f);

    // Blending half way from rest to frame 4 puts the root a unit along.
    TreePose from;
    from.rest(tree);
    pose.evaluate(tree, 0, 4.0f);
    pose.blend(from, 0.5f);
    REQUIRE(pose.poses()[0].position.x == Approx(1.0f));
    REQUIRE(near(Vec3{pose.matrices()[1][3]}, Vec3{2.0f, 2.0f, 0.0f}));

    // Angles blend along the shortest arc across the wrap.
    TreeInfo turning = sampleTree();
    turning.sequences[0].tracks[0] = track(0, 1, {0}, {3.0f});
    TreePose a;
    a.evaluate(turning, 0, 0.0f);
    turning.sequences[0].tracks[0] = track(0, 1, {0}, {-3.0f});
    TreePose b;
    b.evaluate(turning, 0, 0.0f);
    b.blend(a, 0.5f);
    REQUIRE(std::abs(b.poses()[0].rotation.y) == Approx(kPi).margin(1e-4f));
}

TEST_CASE("the angles read off a node's rotation are those of the yaw-pitch-roll order, and "
          "set them back pitch first",
          "[world][animation][pose][gaze]") {
    // Built yaw-pitch-roll, the angles read back rebuild the same matrix; within a right
    // angle of yaw they are the angles that went in (past it, the equivalent triple).
    for (const Vec3 angles : {Vec3{0.4f, 0.7f, -0.3f}, Vec3{-1.2f, 2.5f, 0.9f},
                              Vec3{0.0f, -0.6f, 0.0f}, Vec3{0.3f, 3.0f, -2.8f}}) {
        NodePose pose;
        pose.rotation = angles;
        const Mat4 built = TreePose::localMatrix(pose, Vec3{0.0f});
        const Vec3 read = TreePose::readAngles(built);
        NodePose again = pose;
        again.rotation = read;
        const Mat4 rebuilt = TreePose::localMatrix(again, Vec3{0.0f});
        CAPTURE(angles.x, angles.y, angles.z, read.x, read.y, read.z);
        for (s32 c = 0; c < 4; ++c) {
            REQUIRE(glm::all(glm::epsilonEqual(rebuilt[c], built[c], 1e-4f)));
        }
        if (std::abs(angles.y) < kHalfPi) {
            REQUIRE(near(read, angles));
        }
    }
    // A yaw of a right angle folds pitch and roll together: the roll is left at nought.
    NodePose locked;
    locked.rotation = Vec3{0.5f, kHalfPi, 0.0f};
    const Vec3 read = TreePose::readAngles(TreePose::localMatrix(locked, Vec3{0.0f}));
    REQUIRE(read.z == 0.0f);
    REQUIRE(read.y == Approx(kHalfPi));
    REQUIRE(read.x == Approx(0.5f));

    // A node's read angles set back pitch first keep a single turn as it was; the original
    // rebuilds its look nodes this way (NodeLookAtPos), whatever order their tracks key.
    TreeInfo tree = sampleTree();
    tree.sequences[0].tracks.push_back(track(1, 1, {0}, {0.7f}));
    tree.sequences[0].trackOfNode = {0, 1};
    TreePose pose;
    pose.evaluate(tree, 0, 0.0f);
    REQUIRE_FALSE(pose.poses()[1].pitchYawRoll);
    const Mat4 before = pose.matrices()[1];
    const Vec3 angles = pose.readAngles(1);
    REQUIRE(near(angles, Vec3{0.0f, 0.7f, 0.0f}));
    pose.setPitchYawRoll(1, angles);
    REQUIRE(pose.poses()[1].pitchYawRoll);
    REQUIRE(near(pose.matrices()[1], before));
    // Turned further about the upright, the node's matrix follows and keeps its place.
    pose.setPitchYawRoll(1, angles + Vec3{0.0f, 0.5f, 0.0f});
    REQUIRE(near(Vec3{pose.matrices()[1][3]}, Vec3{before[3]}));
    REQUIRE_FALSE(near(pose.matrices()[1], before));
    REQUIRE(pose.readAngles(1).y == Approx(angles.y + 0.5f));
    // A pitch-first pose reads back as itself only for a turn about one axis; with two the
    // read is the other order's, as the original's is.
    NodePose pitched;
    pitched.rotation = Vec3{0.4f, 0.0f, 0.0f};
    pitched.pitchYawRoll = true;
    REQUIRE(
        near(TreePose::readAngles(TreePose::localMatrix(pitched, Vec3{0.0f})), pitched.rotation));
}

} // namespace
