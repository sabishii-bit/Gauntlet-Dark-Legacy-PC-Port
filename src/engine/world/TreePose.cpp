#include "engine/world/TreePose.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/core/Assert.h"

namespace gdl {

namespace {

/** The pose triple channel 0..8 belongs to. */
Vec3& channelTarget(NodePose& pose, unsigned int channel) {
    if (channel < 3) {
        return pose.rotation;
    }
    return channel < 6 ? pose.position : pose.scale;
}

/** Reads one key's channels into a pose, leaving unkeyed channels at rest. */
NodePose keyPose(const TrackInfo& track, std::size_t key) {
    NodePose pose;
    pose.pitchYawRoll = track.pitchYawRoll();
    const unsigned int channels = track.channelCount();
    std::size_t at = key * channels;
    for (unsigned int c = 0; c < TrackInfo::kChannelCount; ++c) {
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

float TreePose::wrapAngle(float angle) {
    if (angle > kPi) {
        return angle - kTwoPi;
    }
    if (angle <= -kPi) {
        return angle + kTwoPi;
    }
    return angle;
}

NodePose TreePose::sample(const TrackInfo& track, float frame) {
    GDL_VERIFY(!track.frames.empty(), "a track needs at least one key");
    // The key at or after the frame, and the one before it; past the end the last key holds.
    const auto upper =
        std::ranges::lower_bound(track.frames, frame, [](std::uint16_t key, float f) {
            return static_cast<float>(key) < f;
        });
    std::size_t next = static_cast<std::size_t>(upper - track.frames.begin());
    if (next >= track.frames.size()) {
        next = track.frames.size() - 1;
        frame = static_cast<float>(track.frames[next]);
    }
    const std::size_t current = next > 0 ? next - 1 : next;
    const auto currentFrame = static_cast<float>(track.frames[current]);
    const auto nextFrame = static_cast<float>(track.frames[next]);
    NodePose to = keyPose(track, next);
    if (currentFrame < nextFrame && nextFrame - frame > kKeyWindow) {
        const NodePose from = keyPose(track, current);
        const float t = (frame - currentFrame) / (nextFrame - currentFrame);
        for (int i = 0; i < 3; ++i) {
            const float step = to.rotation[i] - from.rotation[i];
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
    const float c0 = std::cos(pose.rotation.x);
    const float s0 = -std::sin(pose.rotation.x);
    const float c1 = std::cos(pose.rotation.y);
    const float s1 = -std::sin(pose.rotation.y);
    const float c2 = std::cos(pose.rotation.z);
    const float s2 = -std::sin(pose.rotation.z);
    Mat4 matrix{1.0f};
    const std::span<float, 16> m(glm::value_ptr(matrix), 16);
    if (pose.pitchYawRoll) {
        const float a = s0 * s1;
        const float b = c0 * s1;
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
        const float a = -c2 * s1;
        const float b = -s2 * s1;
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
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t row = 0; row < 3; ++row) {
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

void TreePose::evaluate(const TreeInfo& tree, unsigned int sequence, float frame, bool mirror) {
    GDL_VERIFY(sequence < tree.sequences.size(), "animation sequence index out of range");
    m_tree = &tree;
    m_poses.assign(tree.nodes.size(), NodePose{});
    const TreeSequenceInfo& info = tree.sequences[sequence];
    for (std::size_t n = 0; n < tree.nodes.size(); ++n) {
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

void TreePose::blend(const TreePose& from, float t) {
    GDL_VERIFY(m_tree != nullptr && from.m_tree == m_tree, "poses to blend must share a tree");
    for (std::size_t n = 0; n < m_poses.size(); ++n) {
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
    for (std::size_t n = 0; n < m_poses.size(); ++n) {
        const TreeNodeInfo& node = m_tree->nodes[n];
        const Mat4 local = localMatrix(m_poses[n], node.position);
        m_matrices[n] = node.parent >= 0 && static_cast<std::size_t>(node.parent) < n
                            ? m_matrices[static_cast<std::size_t>(node.parent)] * local
                            : local;
    }
}

} // namespace gdl
