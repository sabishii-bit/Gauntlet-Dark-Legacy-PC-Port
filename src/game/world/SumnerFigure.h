#pragma once

#include <array>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/**
 * Sumner the good wizard as he stands in his tower: the level's item archive's figure at the
 * lookout the level marks for him, idling through his stance, his reading and his thinking
 * one after the other the way the original cycles them, and gesturing at once when asked.
 */
class SumnerFigure {
public:
    static constexpr std::string_view kTree = "GWIZ";
    static constexpr std::array<std::string_view, 3> kIdleSequences{"READY", "READING", "THINKING"};
    static constexpr std::string_view kGesture = "GESTRIGHT";
    static constexpr s32 kGestureIndex = 6; ///< the original's index for the welcome gesture
    static constexpr u32 kLookout = 0;      ///< the event marker parameter naming his spot

    /** Builds the figure from `items`, which must outlive it, and stands it at the layout's
     * lookout facing back along the marker's heading; false (with a warning) when the
     * archive, the tree or the marker is missing. */
    bool load(RenderDevice& device, ItemArchive& items, const WorldLayout& layout);
    void clear();
    bool loaded() const { return m_tree != nullptr; }

    /** Cuts to the welcome gesture; the idle cycle resumes from the stance after it. */
    void gesture();
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    const Vec3& position() const { return m_position; }
    f32 yaw() const { return m_yaw; }
    /** The sequence playing, and the index the cycle asks for next. */
    u32 sequence() const { return m_player.sequence(); }
    s32 index() const { return m_index; }
    bool gesturing() const {
        return m_gestureSequence >= 0 && sequence() == static_cast<u32>(m_gestureSequence);
    }

private:
    u32 sequenceFor(s32 index) const;

    TreeModel m_model;
    const TreeInfo* m_tree = nullptr;
    std::array<s32, 3> m_idles{-1, -1, -1};
    s32 m_gestureSequence = -1;
    s32 m_index = 0;
    bool m_cutIn = false; ///< the next change starts at once rather than at the end
    AnimationPlayer m_player;
    TreePose m_pose;
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    f32 m_yaw = 0.0f;
    Mat4 m_transform{1.0f};
};

} // namespace gdl::game
