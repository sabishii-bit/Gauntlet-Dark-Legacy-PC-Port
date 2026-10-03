#include "engine/world/TreePose.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

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

f32 TreePose::wrapAngle(f32 angle) {
    if (angle > kPi) {
        return angle - kTwoPi;
    }
    if (angle <= -kPi) {
        return angle + kTwoPi;
    }
    return angle;
}

NodePose TreePose::sample(const TrackInfo& track, f32 frame, bool smooth) {
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
    if (currentFrame < nextFrame && nextFrame - frame > (smooth ? 0.0f : kKeyWindow)) {
        const NodePose from = keyPose(track, current);
        const f32 t = (frame - currentFrame) / (nextFrame - currentFrame);
        for (s32 i = 0; i < 3; ++i) {
            const f32 step = to.rotation[i] - from.rotation[i];
            to.rotation[i] =
                std::abs(step) < kHoldAngle ? from.rotation[i] + step * t : from.rotation[i];
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

void TreePose::evaluate(const TreeInfo& tree, u32 sequence, f32 frame, bool mirror, bool smooth) {
    GDL_VERIFY(sequence < tree.sequences.size(), "animation sequence index out of range");
    m_tree = &tree;
    m_poses.assign(tree.nodes.size(), NodePose{});
    const TreeSequenceInfo& info = tree.sequences[sequence];
    for (usize n = 0; n < tree.nodes.size(); ++n) {
        const TrackInfo* track = info.track(n);
        if (track == nullptr) {
            continue;
        }
        NodePose pose = sample(*track, frame, smooth);
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

void TreePose::blend(const TreePose& from, f32 t, bool preserveCuts) {
    GDL_VERIFY(m_tree != nullptr && from.m_tree == m_tree, "poses to blend must share a tree");
    for (usize n = 0; n < m_poses.size(); ++n) {
        const NodePose& a = from.m_poses[n];
        NodePose& b = m_poses[n];
        for (s32 i = 0; i < 3; ++i) {
            if (a.rotation[i] != b.rotation[i]) {
                const f32 step = wrapAngle(b.rotation[i] - a.rotation[i]);
                if (!preserveCuts || std::abs(step) < kHoldAngle) {
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
    m_poses[node].pitchYawRoll = true;
    compose();
}

Vec3 TreePose::readAngles(const Mat4& rotation) {
    constexpr f32 kLockedYaw = 0.0001f;
    const std::span<const f32, 16> m(glm::value_ptr(rotation), 16);
    if (std::abs(1.0f - std::abs(m[2])) < kLockedYaw) {
        // Looking straight along the side: pitch and roll fold together, roll left at nought.
        return Vec3{std::atan2(m[9], m[5]), m[2] > 0.0f ? -kHalfPi : kHalfPi, 0.0f};
    }
    const f32 pitch = std::atan2(-m[6], m[10]);
    const f32 magnitude = std::cos(pitch);
    if (magnitude == 0.0f) {
        return pitch > 0.0f ? Vec3{pitch, std::atan2(-m[2], -m[6]), std::atan2(-m[8], -m[9])}
                            : Vec3{pitch, std::atan2(-m[2], m[6]), std::atan2(m[8], m[9])};
    }
    const f32 scaled = m[10] / magnitude;
    return Vec3{pitch, std::atan2(-m[2], scaled), std::atan2(-m[1] / scaled, m[0] / scaled)};
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
