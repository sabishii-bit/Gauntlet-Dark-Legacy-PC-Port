#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/math/Math.h"

namespace gdl {

/** One node's transform at a frame: Euler angles, offset from its rest position and scale. */
struct NodePose {
    Vec3 rotation{0.0f, 0.0f, 0.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool pitchYawRoll = false; ///< rotation order, from the track
};

/**
 * Every node of a tree at one frame of a sequence, and the matrix that places each node's
 * mesh in model space: keys are found and interpolated the way the original engine does it
 * (angles only across steps smaller than a right angle), each node's matrix is built in its
 * rotation order and composed down the hierarchy from the rest positions.
 */
class TreePose {
public:
    static constexpr float kHoldAngle = kHalfPi; ///< angle steps this large are not interpolated
    static constexpr float kKeyWindow = 0.125f;  ///< nearer a key than this shows the key itself

    /** Poses the tree at `frame` of `sequence`; nodes without keys stay at rest. Mirroring
     * flips the pose across the model's x axis. */
    void evaluate(const TreeInfo& tree, unsigned int sequence, float frame, bool mirror = false);
    /** Poses the tree at rest. */
    void rest(const TreeInfo& tree);
    /** Blends this pose `t` of the way from `from` (0) to itself (1), angles by the shortest
     * arc, then rebuilds the matrices. Both poses must be of the same tree. */
    void blend(const TreePose& from, float t);

    bool posed() const { return m_tree != nullptr; }
    std::size_t size() const { return m_poses.size(); }
    std::span<const NodePose> poses() const { return m_poses; }
    /** Per node, its local transform composed with every ancestor's. */
    std::span<const Mat4> matrices() const { return m_matrices; }

    /** The pose a track gives at `frame`, past its last key holding that key. */
    static NodePose sample(const TrackInfo& track, float frame);
    /** The rotation, translation (rest position plus the pose's) and scale as one matrix. */
    static Mat4 localMatrix(const NodePose& pose, const Vec3& restPosition);
    /** An angle wrapped into (-pi, pi]. */
    static float wrapAngle(float angle);

private:
    void compose();

    const TreeInfo* m_tree = nullptr;
    std::vector<NodePose> m_poses;
    std::vector<Mat4> m_matrices;
};

} // namespace gdl
