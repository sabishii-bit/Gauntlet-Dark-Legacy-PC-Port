#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {
class NativeStateDigest;
struct CameraFrame;

/** One node's transform at a frame: Euler angles, offset from its rest position and scale. */
struct NodePose {
    enum class RotationOrder : u8 { RollYawPitch = 0, PitchYawRoll = 1, YawPitchRoll = 2 };
    Vec3 rotation{0.0f, 0.0f, 0.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    RotationOrder rotationOrder = RotationOrder::RollYawPitch;
    bool operator==(const NodePose&) const = default;
};

/**
 * Every node of a tree at one frame of a sequence, and the matrix that places each node's
 * mesh in model space: keys are found and interpolated the way the original engine does it
 * (angles only across steps smaller than a right angle), each node's matrix is built in its
 * rotation order and composed down the hierarchy from the rest positions.
 */
class TreePose {
public:
    /** Same-binding local poses and composed gameplay matrices, without
     * serializing resource identities or rebuilding through checkpoint DTOs. */
    bool equalLocal(const TreePose& other) const { return *this == other; }
    bool operator==(const TreePose&) const = default;
    static constexpr f32 kHoldAngle = kHalfPi; ///< angle steps this large are not interpolated
    static constexpr f32 kKeyWindow = 0.125f;  ///< nearer a key than this shows the key itself

    /** How a track is read between its keys: Snapped, as gameplay reads it, shows the next
     * key itself within kKeyWindow of it; Smooth, for presentation only, interpolates right up
     * to the key. Both keep authored large-angle cuts. */
    enum class Sampling : u8 { Snapped, Smooth };
    /** Whether a pose is flipped across the model's x axis. */
    enum class Mirror : u8 { None, AcrossX };
    /** What a blend does with an angle step of kHoldAngle or more: Blend turns across it by
     * the shortest arc; Preserve, for presentation interpolation, keeps the authored cut. */
    enum class Cuts : u8 { Blend, Preserve };

    /** Exact local transforms with a value-only hierarchy identity. Rest offsets
     * belong to that identity: equal node counts alone do not make poses compatible. */
    struct Checkpoint {
        static constexpr usize kMaxNodes = 256;
        static constexpr usize kMaxName = 64;
        static constexpr f32 kMaxMagnitude = 1'000'000;
        struct Node {
            std::string name;
            s32 parent = -1;
            Vec3 rest{0};
            bool operator==(const Node&) const = default;
        };
        std::string tree;
        std::vector<Node> nodes;
        std::vector<NodePose> poses;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    std::optional<Checkpoint> checkpoint() const;
    /** Resolves only against the caller's already trusted tree. Invalid values,
     * changed topology and overflowing composed transforms leave this pose intact. */
    bool restore(const TreeInfo& tree, const Checkpoint& state);

    /** Poses the tree at `frame` of `sequence`, reading each track as `sampling` says and
     * flipping the pose when `mirror` asks; nodes without keys stay at rest. */
    void evaluate(const TreeInfo& tree, u32 sequence, f32 frame,
                  Sampling sampling = Sampling::Snapped, Mirror mirror = Mirror::None);
    /** Poses the tree at rest. */
    void rest(const TreeInfo& tree);
    /** Blends this pose `t` of the way from `from` (0) to itself (1), angles by the shortest
     * arc unless `cuts` keeps a large-angle cut, then rebuilds the matrices. Both poses must
     * be of the same tree. */
    void blend(const TreePose& from, f32 t, Cuts cuts = Cuts::Blend);
    /** Copies local animation within a branch, preserving this pose's other branches. */
    void overlaySubtree(const TreePose& from, usize root);
    /** The pitch, yaw and roll the original reads off node `node`'s local rotation
     * (`readAngles`), whatever order its track keys it in. */
    Vec3 readAngles(usize node) const;
    /** Turns node `node` to `angles` (pitch, yaw, roll, applied in that order) and rebuilds
     * the matrices under it. */
    void setPitchYawRoll(usize node, const Vec3& angles);
    /** Replaces one local pose while allowing its ancestors to keep animating. */
    void setNodePose(usize node, const NodePose& pose);

    bool posed() const { return m_tree != nullptr; }
    usize size() const { return m_poses.size(); }
    std::span<const NodePose> poses() const { return m_poses; }
    /** Per node, its local transform composed with every ancestor's. */
    std::span<const Mat4> matrices() const { return m_matrices; }
    /** World-space draw matrices: facing is applied at each node before its
     * children, including meshless parents. Physics keeps the unfaced pose. */
    std::vector<Mat4> drawMatrices(const Mat4& model, const CameraFrame& camera) const;

    /** The pose a track gives at `frame`, past its last key holding that key. */
    static NodePose sample(const TrackInfo& track, f32 frame,
                           Sampling sampling = Sampling::Snapped);
    /** The frames a repeating sequence shows as one cycle: `count` of them from `first`.
     * Opening frames that only hold the pose of the next are left out, as is a last frame
     * that repeats the first (a whole turn on, for a rotation), so a spin comes round
     * without a pause. */
    struct Cycle {
        s32 first = 0;
        s32 count = 0;
        bool operator==(const Cycle&) const = default;
    };
    static Cycle cycle(const TreeSequenceInfo& sequence);
    /** The rotation, translation (rest position plus the pose's) and scale as one matrix. */
    static Mat4 localMatrix(const NodePose& pose, const Vec3& restPosition);
    /** An angle wrapped into (-pi, pi]. */
    static f32 wrapAngle(f32 angle);
    /** The pitch, yaw and roll the original's ExtractPYR reads off a rotation matrix: exact
     * for one built in yaw-pitch-roll order (the order most tracks key), and what the
     * original then rebuilds pitch-yaw-roll with (NodeLookAtPos); a yaw within a
     * ten-thousandth of a right angle locks the roll at nought. */
    static Vec3 readAngles(const Mat4& rotation);

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const TreePose& /*state*/);
    void compose();

    const TreeInfo* m_tree = nullptr;
    std::vector<NodePose> m_poses;
    std::vector<Mat4> m_matrices;
};

} // namespace gdl
