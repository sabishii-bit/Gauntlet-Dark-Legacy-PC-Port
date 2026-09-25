#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/** Something solid a level's item puts in the way: a box or an upright cylinder. */
struct Obstacle {
    Vec3 centre{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    f32 halfAcross = 0.0f; ///< along its own x
    f32 halfAlong = 0.0f;  ///< along its own z
    f32 height = 0.0f;
    bool solid = true;
    f32 cylinderRadius = 0.0f; ///< positive selects a cylinder instead of the box

    /** Where a body of `radius` standing at `position` is pushed to so as not to be in the
     * box; `position` itself when it is clear of it, over it or under it. */
    Vec3 pushOut(const Vec3& position, f32 radius) const;
    /** Whether a body of `radius` at `position` is against the box, within `margin`. */
    bool touchedBy(const Vec3& position, f32 radius, f32 margin = 0.3f) const;
    /** Swept line of the given radius, respecting height, shape, yaw and solidity. */
    bool blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const;
};

/**
 * One of a level's items as it stands in the world: its figure from the realm's item archive
 * (found by the item's name), playing one of the figure's sequences by its place in the
 * tree, on a loop or once, where the instance puts it on the floor.
 */
class ItemFigure {
public:
    static constexpr f32 kFloorLift = 0.1f;

    /** Stands the figure `name` of `items` (which must outlive it) where `instance` is, on
     * the floor the collision finds; false when the archive has no such figure, in which
     * case the item still has its place and its obstacle. */
    bool place(RenderDevice& device, ItemArchive& items, std::string_view name,
               const ItemInstance& instance, const WorldCollision* collision);

    /** Starts the figure's sequence number `index`. */
    void play(s32 index, bool loop);
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 alpha = 1.0f, f32 scale = 1.0f) const;

    bool hasFigure() const { return m_tree != nullptr; }
    s32 sequence() const { return m_index; }
    /** Whether a sequence played once has reached its end (a missing one has at once). */
    bool finished() const;
    /** Current posed attachment, including the instance's placement. */
    std::optional<Mat4> nodeTransform(std::string_view name) const;
    f32 progress() const;
    /** How many sequences the figure has. */
    usize sequenceCount() const { return m_tree != nullptr ? m_tree->sequences.size() : 0; }
    /** How long the sequence number `index` lasts, in ticks of a sixtieth. */
    s32 ticksOf(s32 index) const;
    const Vec3& position() const { return m_position; }
    f32 yaw() const { return m_yaw; }
    /** The box the item's record gives it, where the figure stands. */
    Obstacle obstacle(const ItemInfo& info) const;

private:
    const TreeInfo* m_tree = nullptr;
    TreeModel m_model;
    TreePose m_pose;
    AnimationPlayer m_player;
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    f32 m_yaw = 0.0f;
    Mat4 m_transform{1.0f};
    s32 m_index = -1;
    bool m_loop = false;
    bool m_holdPose = false;
};

/** Whether a party of `players` sees an item placed for `minPlayers`, by the original's
 * rule: at least that many, or exactly ten less than it when it is over ten. */
bool shownToParty(s32 minPlayers, s32 players);

/** Where an item instance stands: its pitch, yaw and roll as the original stacks them onto
 * its matrix, the yaw outermost (so a half turn of both others is a half turn of yaw). */
Mat4 itemPlacement(const Vec3& position, const Vec3& rotation);

} // namespace gdl::game
