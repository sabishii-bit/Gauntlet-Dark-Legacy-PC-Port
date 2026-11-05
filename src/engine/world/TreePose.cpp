#include "engine/world/TreePose.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"
#include "engine/world/WorldCamera.h"

namespace gdl {

void visitNativeState(NativeStateDigest& digest, const TreePose& state) {
    const auto& [tree, poses, matrices] = state;
    digest.resource(tree);
    digest.wide(poses.size());
    for (const auto& pose : poses) {
        digest.fields<4>(pose);
    }
    digest.value(matrices);
}

namespace {

bool checkpointVector(const Vec3& value) {
    for (s32 axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(value[axis]) ||
            std::abs(value[axis]) > TreePose::Checkpoint::kMaxMagnitude) {
            return false;
        }
    }
    return true;
}

bool checkpointName(const std::string& value) {
    return value.size() <= TreePose::Checkpoint::kMaxName && value.find('\0') == std::string::npos;
}

/** The pose triple channel 0..8 belongs to. */
Vec3& channelTarget(NodePose& pose, u32 channel) {
    if (channel < 3) {
        return pose.rotation;
    }
    return channel < 6 ? pose.position : pose.scale;
}

/** Reads one key's channels into a pose, leaving unkeyed channels at rest. */
NodePose keyPose(const TrackInfo& track, usize key) {
    NodePose pose;
    pose.rotationOrder = (track.flags & TrackInfo::kYawPitchRoll) != 0
                             ? NodePose::RotationOrder::YawPitchRoll
                         : track.pitchYawRoll() ? NodePose::RotationOrder::PitchYawRoll
                                                : NodePose::RotationOrder::RollYawPitch;
    const u32 channels = track.channelCount();
    usize at = key * channels;
    for (u32 c = 0; c < TrackInfo::kChannelCount; ++c) {
        if (!track.has(c)) {
            continue;
        }
        channelTarget(pose, c)[static_cast<s32>(c % 3)] = track.values[at++];
    }
    return pose;
}

Vec3 wrapAngles(Vec3 angles) {
    for (s32 i = 0; i < 3; ++i) {
        angles[i] = TreePose::wrapAngle(angles[i]);
    }
    return angles;
}

} // namespace

bool TreePose::Checkpoint::valid() const {
    if (!checkpointName(tree) || nodes.empty() || nodes.size() > kMaxNodes ||
        nodes.size() != poses.size()) {
        return false;
    }
    for (usize index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const auto& pose = poses[index];
        if (!checkpointName(node.name) || node.parent < -1 ||
            node.parent >= static_cast<s32>(index) || !checkpointVector(node.rest) ||
            pose.rotationOrder > NodePose::RotationOrder::YawPitchRoll ||
            !checkpointVector(pose.rotation) || !checkpointVector(pose.position) ||
            !checkpointVector(pose.scale)) {
            return false;
        }
    }
    return true;
}

std::optional<TreePose::Checkpoint> TreePose::checkpoint() const {
    if (m_tree == nullptr || m_tree->nodes.size() != m_poses.size() ||
        m_poses.size() > Checkpoint::kMaxNodes) {
        return std::nullopt;
    }
    Checkpoint state;
    state.tree = m_tree->name;
    state.poses = m_poses;
    state.nodes.reserve(m_tree->nodes.size());
    for (const auto& node : m_tree->nodes) {
        state.nodes.push_back({node.name, node.parent, node.position});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

bool TreePose::restore(const TreeInfo& tree, const Checkpoint& state) {
    if (!state.valid() || tree.name != state.tree || tree.nodes.size() != state.nodes.size()) {
        return false;
    }
    for (usize index = 0; index < state.nodes.size(); ++index) {
        const auto& expected = tree.nodes[index];
        const auto& node = state.nodes[index];
        if (expected.name != node.name || expected.parent != node.parent ||
            expected.position != node.rest) {
            return false;
        }
    }
    TreePose candidate;
    candidate.m_tree = &tree;
    candidate.m_poses = state.poses;
    candidate.compose();
    for (const auto& matrix : candidate.m_matrices) {
        for (const auto value : std::span<const f32, 16>{glm::value_ptr(matrix), 16}) {
            if (!std::isfinite(value) || std::abs(value) > Checkpoint::kMaxMagnitude) {
                return false;
            }
        }
    }
    *this = std::move(candidate);
    return true;
}

f32 TreePose::wrapAngle(f32 angle) {
    if (angle > kPi) {
        return angle - kTwoPi;
    }
    if (angle <= -kPi) {
        return angle + kTwoPi;
    }
    return angle;
}

NodePose TreePose::sample(const TrackInfo& track, f32 frame, Sampling sampling) {
    GDL_VERIFY(!track.frames.empty(), "a track needs at least one key");
    // The key at or after the frame, and the one before it; past the end the last key holds.
    const auto upper = std::ranges::lower_bound(
        track.frames, frame, [](u16 key, f32 f) { return static_cast<f32>(key) < f; });
    usize next = static_cast<usize>(upper - track.frames.begin());
    if (next >= track.frames.size()) {
        next = track.frames.size() - 1;
        frame = static_cast<f32>(track.frames[next]);
    }
    const usize current = next > 0 ? next - 1 : next;
    const auto currentFrame = static_cast<f32>(track.frames[current]);
    const auto nextFrame = static_cast<f32>(track.frames[next]);
    NodePose to = keyPose(track, next);
    // Presentation can sample continuously; native gameplay retains its key snap window.
    const bool linearAngles = (track.flags & TrackInfo::kLinearAngles) != 0;
    if (currentFrame < nextFrame &&
        nextFrame - frame > (sampling == Sampling::Smooth || linearAngles ? 0.0f : kKeyWindow)) {
        const NodePose from = keyPose(track, current);
        const f32 t = (frame - currentFrame) / (nextFrame - currentFrame);
        for (s32 i = 0; i < 3; ++i) {
            const f32 step = to.rotation[i] - from.rotation[i];
            to.rotation[i] = linearAngles || std::abs(step) < kHoldAngle
                                 ? from.rotation[i] + step * t
                                 : from.rotation[i];
        }
        to.position = from.position + (to.position - from.position) * t;
        to.scale = from.scale + (to.scale - from.scale) * t;
    }
    to.rotation = wrapAngles(to.rotation);
    return to;
}

namespace {
/** How far a closing position or scale may miss the opening one, and the least an angle
 * may. */
constexpr f32 kCycleClosure = 0.02f;

NodePose poseAt(const TrackInfo& track, s32 frame) {
    return TreePose::sample(track, static_cast<f32>(frame), TreePose::Sampling::Smooth);
}

/** How far a track turns from one frame to the next on average, from `first` on: the largest
 * of its angles' steps. */
f32 typicalTurn(const TrackInfo& track, s32 first, s32 frames) {
    f32 total = 0.0f;
    for (s32 frame = first + 1; frame < frames; ++frame) {
        const NodePose before = poseAt(track, frame - 1);
        const NodePose after = poseAt(track, frame);
        f32 largest = 0.0f;
        for (s32 i = 0; i < 3; ++i) {
            largest = std::max(
                largest, std::abs(TreePose::wrapAngle(after.rotation[i] - before.rotation[i])));
        }
        total += largest;
    }
    return frames - first > 1 ? total / static_cast<f32>(frames - first - 1) : 0.0f;
}

/** Whether the last frame's pose repeats the first's: each angle a turn on within half a
 * frame's turn, each position and scale within the closure. */
bool closes(const NodePose& last, const NodePose& first, f32 turn) {
    const f32 slack = std::max(kCycleClosure, 0.5f * turn);
    for (s32 i = 0; i < 3; ++i) {
        if (std::abs(TreePose::wrapAngle(last.rotation[i] - first.rotation[i])) > slack ||
            std::abs(last.position[i] - first.position[i]) > kCycleClosure ||
            std::abs(last.scale[i] - first.scale[i]) > kCycleClosure) {
            return false;
        }
    }
    return true;
}
} // namespace

TreePose::Cycle TreePose::cycle(const TreeSequenceInfo& sequence) {
    const Cycle whole{0, std::max(sequence.frames, 0)};
    if (sequence.frames <= 1) {
        return whole;
    }
    // Only a sequence that moves a node has holds to leave out; a still one, or one that
    // only turns flip-book pages, shows every frame.
    const auto moves = [&](const TrackInfo& track) {
        const NodePose start = poseAt(track, 0);
        for (s32 frame = 1; frame < sequence.frames; ++frame) {
            if (poseAt(track, frame) != start) {
                return true;
            }
        }
        return false;
    };
    if (std::ranges::none_of(sequence.tracks, moves)) {
        return whole;
    }
    const auto holds = [&](s32 frame, s32 other) {
        return std::ranges::all_of(sequence.tracks, [&](const TrackInfo& track) {
            return poseAt(track, frame) == poseAt(track, other);
        });
    };
    Cycle cycle = whole;
    while (cycle.first + 1 < sequence.frames && holds(cycle.first, cycle.first + 1)) {
        ++cycle.first;
    }
    cycle.count = sequence.frames - cycle.first;
    const bool closed =
        cycle.count > 1 && std::ranges::all_of(sequence.tracks, [&](const TrackInfo& track) {
            return closes(poseAt(track, sequence.frames - 1), poseAt(track, cycle.first),
                          typicalTurn(track, cycle.first, sequence.frames));
        });
    if (closed) {
        --cycle.count;
    }
    return cycle;
}

Mat4 TreePose::localMatrix(const NodePose& pose, const Vec3& restPosition) {
    // The original builds its matrices with negated sines; kept as written so every joint
    // turns the way the data expects.
    const f32 c0 = portable::cos(pose.rotation.x);
    const f32 s0 = -portable::sin(pose.rotation.x);
    const f32 c1 = portable::cos(pose.rotation.y);
    const f32 s1 = -portable::sin(pose.rotation.y);
    const f32 c2 = portable::cos(pose.rotation.z);
    const f32 s2 = -portable::sin(pose.rotation.z);
    Mat4 matrix{1.0f};
    const std::span<f32, 16> m(glm::value_ptr(matrix), 16);
    if (pose.rotationOrder == NodePose::RotationOrder::YawPitchRoll) {
        // N64 Legends e0014d04. Unlike either GameCube order, yaw is applied
        // before pitch. Keep the same negated-sine convention as the other paths.
        m[0] = c1 * c2 - s1 * s0 * s2;
        m[1] = c0 * s2;
        m[2] = s1 * c2 + c1 * s0 * s2;
        m[4] = -c1 * s2 - s1 * s0 * c2;
        m[5] = c0 * c2;
        m[6] = -s1 * s2 + c1 * s0 * c2;
        m[8] = -s1 * c0;
        m[9] = -s0;
        m[10] = c1 * c0;
    } else if (pose.rotationOrder == NodePose::RotationOrder::PitchYawRoll) {
        const f32 a = s0 * s1;
        const f32 b = c0 * s1;
        m[0] = c1 * c2;
        m[4] = -c1 * s2;
        m[8] = -s1;
        m[1] = -a * c2 + c0 * s2;
        m[5] = a * s2 + c0 * c2;
        m[9] = -s0 * c1;
        m[2] = b * c2 + s0 * s2;
        m[6] = b * -s2 + s0 * c2;
        m[10] = c0 * c1;
    } else {
        const f32 a = -c2 * s1;
        const f32 b = -s2 * s1;
        m[0] = c2 * c1;
        m[4] = -s2 * c0 + a * s0;
        m[8] = s2 * s0 + a * c0;
        m[1] = s2 * c1;
        m[5] = c2 * c0 + b * s0;
        m[9] = -c2 * s0 + b * c0;
        m[2] = s1;
        m[6] = c1 * s0;
        m[10] = c1 * c0;
    }
    for (usize axis = 0; axis < 3; ++axis) {
        for (usize row = 0; row < 3; ++row) {
            m[axis * 4 + row] *= pose.scale[static_cast<s32>(axis)];
        }
    }
    const Vec3 translation = restPosition + pose.position;
    m[12] = translation.x;
    m[13] = translation.y;
    m[14] = translation.z;
    return matrix;
}

void TreePose::rest(const TreeInfo& tree) {
    m_tree = &tree;
    m_poses.assign(tree.nodes.size(), NodePose{});
    compose();
}

void TreePose::evaluate(const TreeInfo& tree, u32 sequence, f32 frame, Sampling sampling,
                        Mirror mirror) {
    GDL_VERIFY(sequence < tree.sequences.size(), "animation sequence index out of range");
    m_tree = &tree;
    m_poses.assign(tree.nodes.size(), NodePose{});
    const TreeSequenceInfo& info = tree.sequences[sequence];
    for (usize n = 0; n < tree.nodes.size(); ++n) {
        const TrackInfo* track = info.track(n);
        if (track == nullptr) {
            continue;
        }
        NodePose pose = sample(*track, frame, sampling);
        if (mirror == Mirror::AcrossX) {
            pose.rotation.y = -pose.rotation.y;
            pose.rotation.z = -pose.rotation.z;
            pose.position.x = -pose.position.x;
            pose.scale.x = -pose.scale.x;
        }
        m_poses[n] = pose;
    }
    compose();
}

void TreePose::blend(const TreePose& from, f32 t, Cuts cuts) {
    GDL_VERIFY(m_tree != nullptr && from.m_tree == m_tree, "poses to blend must share a tree");
    for (usize n = 0; n < m_poses.size(); ++n) {
        const NodePose& a = from.m_poses[n];
        NodePose& b = m_poses[n];
        for (s32 i = 0; i < 3; ++i) {
            if (a.rotation[i] != b.rotation[i]) {
                const f32 step = wrapAngle(b.rotation[i] - a.rotation[i]);
                if (cuts == Cuts::Blend || std::abs(step) < kHoldAngle) {
                    b.rotation[i] = a.rotation[i] + step * t;
                }
            }
        }
        b.position = a.position + (b.position - a.position) * t;
        b.scale = a.scale + (b.scale - a.scale) * t;
    }
    compose();
}

void TreePose::overlaySubtree(const TreePose& from, usize root) {
    GDL_VERIFY(m_tree != nullptr && from.m_tree == m_tree, "subtree poses must share a tree");
    GDL_VERIFY(root < m_poses.size(), "subtree root out of range");
    for (usize n = root; n < m_poses.size(); ++n) {
        s32 ancestor = static_cast<s32>(n);
        while (ancestor >= 0 && static_cast<usize>(ancestor) != root) {
            ancestor = m_tree->nodes[static_cast<usize>(ancestor)].parent;
        }
        if (ancestor >= 0) {
            m_poses[n] = from.m_poses[n];
        }
    }
    compose();
}

Vec3 TreePose::readAngles(usize node) const {
    GDL_VERIFY(node < m_poses.size(), "node out of range");
    NodePose unscaled = m_poses[node];
    unscaled.scale = Vec3{1.0f, 1.0f, 1.0f};
    unscaled.position = Vec3{0.0f, 0.0f, 0.0f};
    return readAngles(localMatrix(unscaled, Vec3{0.0f, 0.0f, 0.0f}));
}

void TreePose::setPitchYawRoll(usize node, const Vec3& angles) {
    GDL_VERIFY(node < m_poses.size(), "node out of range");
    m_poses[node].rotation = wrapAngles(angles);
    m_poses[node].rotationOrder = NodePose::RotationOrder::PitchYawRoll;
    compose();
}

Vec3 TreePose::readAngles(const Mat4& rotation) {
    constexpr f32 kLockedYaw = 0.0001f;
    const std::span<const f32, 16> m(glm::value_ptr(rotation), 16);
    if (std::abs(1.0f - std::abs(m[2])) < kLockedYaw) {
        // Looking straight along the side: pitch and roll fold together, roll left at nought.
        return Vec3{portable::atan2(m[9], m[5]), m[2] > 0.0f ? -kHalfPi : kHalfPi, 0.0f};
    }
    const f32 pitch = portable::atan2(-m[6], m[10]);
    const f32 magnitude = portable::cos(pitch);
    if (magnitude == 0.0f) {
        return pitch > 0.0f
                   ? Vec3{pitch, portable::atan2(-m[2], -m[6]), portable::atan2(-m[8], -m[9])}
                   : Vec3{pitch, portable::atan2(-m[2], m[6]), portable::atan2(m[8], m[9])};
    }
    const f32 scaled = m[10] / magnitude;
    return Vec3{pitch, portable::atan2(-m[2], scaled),
                portable::atan2(-m[1] / scaled, m[0] / scaled)};
}

void TreePose::setNodePose(usize node, const NodePose& pose) {
    if (node < m_poses.size()) {
        m_poses[node] = pose;
        compose();
    }
}

std::vector<Mat4> TreePose::drawMatrices(const Mat4& model, const CameraFrame& camera) const {
    std::vector<Mat4> matrices(m_poses.size());
    for (usize n = 0; n < m_poses.size(); ++n) {
        const TreeNodeInfo& node = m_tree->nodes[n];
        const Mat4& parent = node.parent >= 0 && static_cast<usize>(node.parent) < n
                                 ? matrices[static_cast<usize>(node.parent)]
                                 : model;
        matrices[n] = camera.face(parent * localMatrix(m_poses[n], node.position),
                                  CameraFrame::facingOf(node.objectFlags));
    }
    return matrices;
}

void TreePose::compose() {
    m_matrices.resize(m_poses.size());
    for (usize n = 0; n < m_poses.size(); ++n) {
        const TreeNodeInfo& node = m_tree->nodes[n];
        const Mat4 local = localMatrix(m_poses[n], node.position);
        m_matrices[n] = node.parent >= 0 && static_cast<usize>(node.parent) < n
                            ? m_matrices[static_cast<usize>(node.parent)] * local
                            : local;
    }
}

} // namespace gdl
