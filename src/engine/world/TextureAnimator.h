#pragma once

#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldScene.h"

namespace gdl {

/** What an animation shows now: the frame its cycle has reached, or how far its scroll has
 * slid a coordinate. */
struct TextureMotion {
    u32 slot = 0;
    const Texture* frame = nullptr; ///< null for a scroll
    Vec2 offset{0.0f, 0.0f};
};

/**
 * A level's texture animations as the original steps them once a game frame: a cycle shows
 * the next of its frames every `rate` frames, and a scroll slides the coordinates a step of
 * its cycle each frame. The frames of a cycle are the set's own entries after its source,
 * or entries found by name in the level's set or the sets other archives lend it.
 */
class TextureAnimator {
public:
    /** Resolves `animations` against the level's textures; those whose frames cannot be
     * found are dropped with a warning. */
    void bind(std::span<const TextureAnimationInfo> animations, TextureSet& textures,
              RenderDevice& device, std::span<TextureSet* const> lenders = {});
    void clear();
    usize size() const { return m_entries.size(); }
    /** Game frames stepped since binding. */
    u32 frame() const { return m_frame; }
    /** Where an animation is in its cycle. */
    s32 counter(usize index) const { return m_entries[index].counter; }
    u32 slot(usize index) const { return m_entries[index].slot; }

    /** Where an animation stands. */
    TextureMotion motion(usize index) const;
    /** Advances `ticks` game frames. */
    void step(u32 ticks = 1);
    /** Shows every animation where it stands. */
    void apply(WorldScene& scene) const;
    /** Advances `ticks` game frames, showing each step. */
    void step(WorldScene& scene, u32 ticks = 1);

private:
    struct Entry {
        u32 slot = 0;
        std::vector<const Texture*> frames; ///< the cycle's textures, when it cycles
        Vec2 direction{0.0f, 0.0f};         ///< the coordinate a scroll slides, and which way
        s32 period = 1;                     ///< steps in a cycle
        s32 rate = 0;                       ///< frames per step; 0 or 1 steps every frame
        s32 counter = 0;
    };

    static void show(const Entry& entry, WorldScene& scene);

    std::vector<Entry> m_entries;
    u32 m_frame = 0;
};

} // namespace gdl
