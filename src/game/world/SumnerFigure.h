#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
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
    /** His sequences in the original's order; the first three are the idle cycle. */
    static constexpr std::array<std::string_view, 7> kSequences{
        "READY", "READING", "THINKING", "WELCOME", "GOAWAY", "GESTLEFT", "GESTRIGHT"};
    static constexpr int kWelcomeIndex = 3;     ///< greeting a player who steps up to him
    static constexpr int kGoAwayIndex = 4;      ///< seeing them off once they are done
    static constexpr int kGestureIndex = 6;     ///< the sweep towards the crystals
    static constexpr unsigned int kLookout = 0; ///< the event marker parameter naming his spot

    /** Builds the figure from `items`, which must outlive it, and stands it at the layout's
     * lookout facing back along the marker's heading; false (with a warning) when the
     * archive, the tree or the marker is missing. */
    bool load(RenderDevice& device, ItemArchive& items, const WorldLayout& layout);
    void clear();
    bool loaded() const { return m_tree != nullptr; }

    /** Cuts to the sweep towards the crystals; the idle cycle resumes from the stance after
     * it. */
    void gesture() { play(kGestureIndex); }
    /** Cuts to one of kSequences at once; the idle cycle resumes after it. */
    void play(int index);
    void update(float seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    const Vec3& position() const { return m_position; }
    float yaw() const { return m_yaw; }
    /** The sequence playing, and the index the cycle asks for next. */
    unsigned int sequence() const { return m_player.sequence(); }
    int index() const { return m_index; }
    bool gesturing() const { return playing(kGestureIndex); }
    /** Whether the sequence of that index is the one playing. */
    bool playing(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < m_sequences.size() &&
               m_sequences[static_cast<std::size_t>(index)] >= 0 &&
               sequence() ==
                   static_cast<unsigned int>(m_sequences[static_cast<std::size_t>(index)]);
    }

private:
    unsigned int sequenceFor(int index) const;

    TreeModel m_model;
    const TreeInfo* m_tree = nullptr;
    std::array<int, kSequences.size()> m_sequences{-1, -1, -1, -1, -1, -1, -1};
    int m_index = 0;
    bool m_cutIn = false; ///< the next change starts at once rather than at the end
    AnimationPlayer m_player;
    TreePose m_pose;
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    float m_yaw = 0.0f;
    Mat4 m_transform{1.0f};
};

} // namespace gdl::game
