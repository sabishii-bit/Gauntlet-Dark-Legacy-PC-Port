#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCamera.h"

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

TEST_CASE("native pose equality checks transforms and their loaded hierarchy",
          "[pose][local-rewind]") {
    const auto tree = sampleTree();
    // A distinct copy must compare unequal.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto otherTree = tree;
    TreePose pose;
    pose.evaluate(tree, 0, 2);
    const auto saved = pose;
    REQUIRE(pose.equalLocal(saved));
    auto node = pose.poses()[1];
    node.rotation.y = 0.25f;
    pose.setNodePose(1, node);
    CHECK_FALSE(pose.equalLocal(saved));
    pose = saved;
    const auto drawn = pose.drawMatrices(Mat4{1}, CameraFrame::of(WorldCamera{}));
    CHECK(drawn.size() == pose.size());
    CHECK(pose.equalLocal(saved));
    TreePose other;
    other.evaluate(otherTree, 0, 2);
    CHECK_FALSE(other.equalLocal(saved));
    other = saved;
    CHECK(other.equalLocal(saved));
}

TEST_CASE("a held node pose keeps following its animated parent", "[world][pose]") {
    const TreeInfo tree = sampleTree();
    TreePose pose;
    pose.rest(tree);
    NodePose held = pose.poses()[1];
    held.position = {0, 3, 0};
    pose.evaluate(tree, 0, 4);
    pose.setNodePose(1, held);
    REQUIRE(near(Vec3{pose.matrices()[1][3]}, Vec3{3, 5, 0}));
    pose.setNodePose(999, held);
    REQUIRE(pose.size() == 2);
}

TEST_CASE("visual sampling removes near-key snapping without blending authored angle cuts",
          "[world][pose][presentation]") {
    const auto slide = track(0, 3, {0, 1}, {0, 10});
    CHECK(TreePose::sample(slide, 0.95f).position.x == 10.0f);
    CHECK(TreePose::sample(slide, 0.95f, true).position.x == Approx(9.5f));
    const auto cut = track(0, 1, {0, 1}, {0, kPi});
    CHECK(TreePose::sample(cut, 0.95f, true).rotation.y == 0.0f);
    CHECK(TreePose::sample(cut, 1.0f, true).rotation.y == Approx(kPi));
    auto tree = sampleTree();
    tree.sequences[0].tracks[0] = slide;
    TreePose native;
    TreePose visual;
    native.evaluate(tree, 0, 0.95f);
    visual.evaluate(tree, 0, 0.95f, false, true);
    CHECK(native.matrices()[0][3].x == 11.0f);
    CHECK(visual.matrices()[0][3].x == Approx(10.5f));
    tree.sequences[0].tracks[0] = cut;
    native.evaluate(tree, 0, 0);
    visual.evaluate(tree, 0, 1, false, true);
    visual.blend(native, 0.5f, true);
    CHECK(visual.poses()[0].rotation.y == Approx(kPi));
}

bool near(const Mat4& a, const Mat4& b) {
    for (s32 c = 0; c < 4; ++c) {
        if (!glm::all(glm::epsilonEqual(a[c], b[c], 1e-5f))) {
            return false;
        }
    }
    return true;
}

TEST_CASE("draw poses inherit facing through meshless parents without changing physics",
          "[world][pose][effects]") {
    TreeInfo tree = sampleTree();
    tree.nodes[0].objectFlags = CameraFrame::kFacingFull << 24;
    tree.nodes[1].position = {2, 1, 3};
    TreePose pose;
    pose.evaluate(tree, 0, 2);
    const Mat4 physics = pose.matrices()[1];
    WorldCamera view;
    view.position = {10, 20, -30};
    view.yaw = 0.8f;
    view.pitch = 0.3f;
    const auto camera = CameraFrame::of(view);
    const Mat4 model = glm::translate(Mat4{1}, Vec3{4, 5, 6});
    for (const f32 scale : {0.0f, 0.5f, 2.0f}) {
        auto root = pose.poses()[0];
        root.scale = Vec3{scale};
        pose.setNodePose(0, root);
        const auto matrices = pose.drawMatrices(model, camera);
        REQUIRE(matrices.size() == 2);
        const Mat4 expectedRoot = camera.face(
            model * TreePose::localMatrix(root, tree.nodes[0].position), CameraFrame::kFacingFull);
        CHECK(near(matrices[1], expectedRoot * glm::translate(Mat4{1}, tree.nodes[1].position)));
    }
    pose.evaluate(tree, 0, 2);
    CHECK(near(pose.matrices()[1], physics));
    tree.nodes[0].objectFlags = 0;
    const auto unfaced = pose.drawMatrices(model, camera);
    CHECK(near(unfaced[1], model * physics));
    CHECK(TreePose{}.drawMatrices(model, camera).empty());
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
    pose.rotationOrder = NodePose::RotationOrder::PitchYawRoll;
    REQUIRE(near(TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f}), pitched));
    // Both orders agree on a single axis; they differ once two combine.
    pose.rotation = Vec3{0.4f, 0.7f, 0.0f};
    const Mat4 pyr = TreePose::localMatrix(pose, Vec3{0.0f, 0.0f, 0.0f});
    pose.rotationOrder = NodePose::RotationOrder::RollYawPitch;
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

TEST_CASE("Legends yaw-pitch-roll is a separate order with continuous native Euler sampling",
          "[world][pose][legends]") {
    NodePose pose;
    pose.rotation = {0.3f, 0.7f, -0.2f};
    pose.rotationOrder = NodePose::RotationOrder::YawPitchRoll;
    const Mat4 expected = glm::rotate(Mat4{1}, pose.rotation.y, Vec3{0, 1, 0}) *
                          glm::rotate(Mat4{1}, -pose.rotation.x, Vec3{1, 0, 0}) *
                          glm::rotate(Mat4{1}, -pose.rotation.z, Vec3{0, 0, 1});
    CHECK(near(TreePose::localMatrix(pose, Vec3{0}), expected));
    auto native = track(0, 0, {0, 1}, {0, 3.0f});
    native.flags |= TrackInfo::kYawPitchRoll | TrackInfo::kLinearAngles;
    const auto sampled = TreePose::sample(native, 0.95f);
    CHECK(sampled.rotationOrder == NodePose::RotationOrder::YawPitchRoll);
    CHECK(sampled.rotation.x == Approx(2.85f));
    native.flags &= static_cast<u16>(~TrackInfo::kLinearAngles);
    CHECK(TreePose::sample(native, 0.5f).rotation.x == 0.0f);

    auto tree = sampleTree();
    TreePose held;
    held.rest(tree);
    held.setNodePose(0, pose);
    auto state = held.checkpoint();
    REQUIRE(state);
    CHECK(state->valid());
    state->poses[0].rotationOrder = static_cast<NodePose::RotationOrder>(3);
    CHECK_FALSE(state->valid());
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
    REQUIRE(pose.poses()[1].rotationOrder == NodePose::RotationOrder::RollYawPitch);
    const Mat4 before = pose.matrices()[1];
    const Vec3 angles = pose.readAngles(1);
    REQUIRE(near(angles, Vec3{0.0f, 0.7f, 0.0f}));
    pose.setPitchYawRoll(1, angles);
    REQUIRE(pose.poses()[1].rotationOrder == NodePose::RotationOrder::PitchYawRoll);
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
    pitched.rotationOrder = NodePose::RotationOrder::PitchYawRoll;
    REQUIRE(
        near(TreePose::readAngles(TreePose::localMatrix(pitched, Vec3{0.0f})), pitched.rotation));
}

TEST_CASE("a sequence's cycle leaves out an opening hold and a closing repeat of its first frame",
          "[world][pose][cycle]") {
    // The reflective shield's spin: thirty-one frames keyed on one yaw, the first held for a
    // frame and the last a whole turn on from it, so the turn runs over twenty-nine frames.
    TreeSequenceInfo closed;
    closed.frames = 31;
    closed.frameRate = 30;
    std::vector<u16> frames;
    std::vector<f32> yaw;
    for (u16 f = 0; f <= 30; ++f) {
        frames.push_back(f);
        yaw.push_back(f == 0 ? 0.0f : static_cast<f32>(f - 1) * (6.2904f / 29.0f));
    }
    closed.tracks.push_back(track(2, 1, frames, yaw));
    CHECK(TreePose::cycle(closed) == TreePose::Cycle{1, 29});
    // A potion's spin: thirty frames, the last a step short of the turn, all shown.
    TreeSequenceInfo open;
    open.frames = 30;
    open.frameRate = 30;
    std::vector<u16> openFrames;
    std::vector<f32> openYaw;
    for (u16 f = 0; f < 30; ++f) {
        openFrames.push_back(f);
        openYaw.push_back(-static_cast<f32>(f) * (6.2832f / 30.0f));
    }
    open.tracks.push_back(track(2, 1, openFrames, openYaw));
    CHECK(TreePose::cycle(open) == TreePose::Cycle{0, 30});
    // A closing frame that also changes scale does not repeat the first.
    TreeSequenceInfo scaled = closed;
    scaled.tracks.push_back(track(0, 6, {0, 1, 30}, {1.0f, 1.0f, 0.5f}));
    CHECK(TreePose::cycle(scaled) == TreePose::Cycle{1, 30});
    // A still figure, and a sequence of flip-book pages with no track at all, show every
    // frame; a single frame is its own cycle.
    TreeSequenceInfo still;
    still.frames = 4;
    still.tracks.push_back(track(0, 1, {0, 3}, {0.5f, 0.5f}));
    CHECK(TreePose::cycle(still) == TreePose::Cycle{0, 4});
    TreeSequenceInfo pages;
    pages.frames = 30;
    CHECK(TreePose::cycle(pages) == TreePose::Cycle{0, 30});
    TreeSequenceInfo one;
    one.frames = 1;
    CHECK(TreePose::cycle(one) == TreePose::Cycle{0, 1});
    // The boss key's turn: thirty-nine frames whose last stops two degrees short of the
    // turn, nearer the first frame than the next step of nine degrees, so it closes; a
    // turn whose last frame is a whole step short stays open.
    TreeSequenceInfo key;
    key.frames = 39;
    std::vector<u16> keyFrames;
    std::vector<f32> keyYaw;
    for (u16 f = 0; f < 39; ++f) {
        keyFrames.push_back(f);
        keyYaw.push_back(static_cast<f32>(f) * 0.164f);
    }
    key.tracks.push_back(track(0, 1, keyFrames, keyYaw));
    CHECK(TreePose::cycle(key) == TreePose::Cycle{0, 38});
    TreeSequenceInfo rune = key;
    rune.tracks[0].values.back() = 38.0f * 0.164f - 0.1f;
    CHECK(TreePose::cycle(rune) == TreePose::Cycle{0, 39});
}

/** Three nodes and five sequences keying all of them, each sequence a little apart. */
TreeInfo checkpointTree() {
    TreeInfo tree;
    tree.name = "POSE";
    for (s32 node = 0; node < 3; ++node) {
        TreeNodeInfo info;
        info.name = "NODE" + std::to_string(node);
        info.parent = node - 1;
        info.position = {0.125f * static_cast<f32>(node), 0.25f, -0.5f};
        tree.nodes.push_back(info);
    }
    constexpr usize kSequences = 5;
    for (usize index = 0; index < kSequences; ++index) {
        TreeSequenceInfo sequence;
        sequence.name = "SEQ" + std::to_string(index);
        sequence.frames = 9;
        sequence.frameRate = 24;
        const auto offset = static_cast<f32>(index) * 0.03125f;
        for (u32 node = 0; node < tree.nodes.size(); ++node) {
            TrackInfo keyed;
            keyed.node = node;
            keyed.flags = static_cast<u16>(TrackInfo::kChannels |
                                           ((node % 2) != 0 ? TrackInfo::kPitchYawRoll : 0));
            keyed.frames = {0, 8};
            keyed.values = {
                offset,        0.2f, -0.3f, offset,        0.5f, 0.75f, 1,    1.125f, 0.875f,
                offset + 0.4f, 0.6f, -0.1f, offset + 0.5f, 0.6f, 0.8f,  0.9f, 1,      1.1f};
            sequence.trackOfNode.push_back(static_cast<s32>(sequence.tracks.size()));
            sequence.tracks.push_back(keyed);
        }
        tree.sequences.push_back(sequence);
    }
    return tree;
}

void samePose(const TreePose& actual, const TreePose& expected) {
    CHECK(std::ranges::equal(actual.poses(), expected.poses()));
    CHECK(std::ranges::equal(actual.matrices(), expected.matrices()));
}

TEST_CASE("bound tree pose checkpoints preserve every local channel and reject changed rosters",
          "[world][pose][rollback][tree-pose-checkpoint]") {
    const auto tree = checkpointTree();
    TreePose original;
    original.evaluate(tree, 4, 3.5f);
    NodePose changed = original.poses()[1];
    changed.rotation = {-0.9f, 1.1f, -0.7f};
    changed.position = {0.4f, -0.3f, 0.2f};
    changed.scale = {-1, 0.8f, 1.2f};
    changed.rotationOrder = NodePose::RotationOrder::PitchYawRoll;
    original.setNodePose(1, changed);
    const auto state = original.checkpoint();
    REQUIRE(state);
    REQUIRE(state->valid());
    TreePose restored;
    REQUIRE(restored.restore(tree, *state));
    samePose(restored, original);
    CHECK(restored.checkpoint() == state);

    const auto reject = [&](const TreeInfo& trusted, const TreePose::Checkpoint& invalid) {
        CHECK_FALSE(restored.restore(trusted, invalid));
        CHECK(restored.checkpoint() == state);
        samePose(restored, original);
    };
    auto other = tree;
    other.name = "ANOTHER";
    reject(other, *state);
    other = tree;
    other.nodes[1].name = "WRONG_NODE";
    reject(other, *state);
    other = tree;
    other.nodes[2].parent = 0;
    reject(other, *state);
    other = tree;
    other.nodes[0].position.x += 0.01f;
    reject(other, *state);
    other = tree;
    std::swap(other.nodes[1], other.nodes[2]);
    reject(other, *state);
    auto invalid = *state;
    invalid.nodes.pop_back();
    reject(tree, invalid);
    invalid = *state;
    invalid.poses[1].position.y = std::numeric_limits<f32>::quiet_NaN();
    reject(tree, invalid);
    invalid = *state;
    invalid.nodes[1].parent = 1;
    reject(tree, invalid);
    invalid = *state;
    invalid.tree.assign(TreePose::Checkpoint::kMaxName + 1, 'a');
    reject(tree, invalid);
    invalid = *state;
    invalid.nodes[1].name.push_back('\0');
    reject(tree, invalid);
    invalid = *state;
    for (auto& pose : invalid.poses) {
        pose.scale = Vec3{TreePose::Checkpoint::kMaxMagnitude};
    }
    REQUIRE(invalid.valid());
    reject(tree, invalid); // finite local scales may overflow after parent composition
}

} // namespace
