#include "engine/world/TreePose.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "engine/core/Assert.h"

namespace gdl {

namespace {

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
    pose.pitchYawRoll = track.pitchYawRoll();
    const u32 channels = track.channelCount();
    usize at = key * channels;
    for (u32 c = 0; c < TrackInfo::kChannelCount; ++c) {
        if (!track.has(c)) {
            continue;
        }
        channelTarget(pose, c)[static_cast<int>(c % 3)] = track.values[at++];
    }
    return pose;
}

Vec3 wrapAngles(Vec3 angles) {
    for (int i = 0; i < 3; ++i) {
        angles[i] = TreePose::wrapAngle(angles[i]);
    }
    return angles;
}

} // namespace

f32 TreePose::wrapAngle(f32 angle) {
    if (angle > kPi) {
        return angle - kTwoPi;
    }
    if (angle <= -kPi) {
        return angle + kTwoPi;
    }
    return angle;
}

NodePose TreePose::sample(const TrackInfo& track, f32 frame) {
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
    if (currentFrame < nextFrame && nextFrame - frame > kKeyWindow) {
        const NodePose from = keyPose(track, current);
        const f32 t = (frame - currentFrame) / (nextFrame - currentFrame);
        for (int i = 0; i < 3; ++i) {
            const f32 step = to.rotation[i] - from.rotation[i];
            to.rotation[i] = std::abs(step) < kHoldAngle ? from.rotation[i] + step * t
                                                         : from.rotation[i];
        }
        to.position = from.position + (to.position - from.position) * t;
        to.scale = from.scale + (to.scale - from.scale) * t;
    }
    to.rotation = wrapAngles(to.rotation);
    return to;
}

Mat4 TreePose::localMatrix(const NodePose& pose, const Vec3& restPosition) {
    // The original builds its matrices with negated sines; kept as written so every joint
    // turns the way the data expects.
    const f32 c0 = std::cos(pose.rotation.x);
    const f32 s0 = -std::sin(pose.rotation.x);
    const f32 c1 = std::cos(pose.rotation.y);
    const f32 s1 = -std::sin(pose.rotation.y);
    const f32 c2 = std::cos(pose.rotation.z);
    const f32 s2 = -std::sin(pose.rotation.z);
    Mat4 matrix{1.0f};
    const std::span<f32, 16> m(glm::value_ptr(matrix), 16);
    if (pose.pitchYawRoll) {
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
            m[axis * 4 + row] *= pose.scale[static_cast<int>(axis)];
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

void TreePose::evaluate(const TreeInfo& tree, u32 sequence, f32 frame, bool mirror) {
    GDL_VERIFY(sequence < tree.sequences.size(), "animation sequence index out of range");
    m_tree = &tree;
    m_poses.assign(tree.nodes.size(), NodePose{});
    const TreeSequenceInfo& info = tree.sequences[sequence];
    for (usize n = 0; n < tree.nodes.size(); ++n) {
        const TrackInfo* track = info.track(n);
        if (track == nullptr) {
            continue;
        }
        NodePose pose = sample(*track, frame);
        if (mirror) {
            pose.rotation.y = -pose.rotation.y;
            pose.rotation.z = -pose.rotation.z;
            pose.position.x = -pose.position.x;
            pose.scale.x = -pose.scale.x;
        }
        m_poses[n] = pose;
    }
    compose();
}

void TreePose::blend(const TreePose& from, f32 t) {
    GDL_VERIFY(m_tree != nullptr && from.m_tree == m_tree, "poses to blend must share a tree");
    for (usize n = 0; n < m_poses.size(); ++n) {
        const NodePose& a = from.m_poses[n];
        NodePose& b = m_poses[n];
        for (int i = 0; i < 3; ++i) {
            if (a.rotation[i] != b.rotation[i]) {
                b.rotation[i] = a.rotation[i] + wrapAngle(b.rotation[i] - a.rotation[i]) * t;
            }
        }
        b.position = a.position + (b.position - a.position) * t;
        b.scale = a.scale + (b.scale - a.scale) * t;
    }
    compose();
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
