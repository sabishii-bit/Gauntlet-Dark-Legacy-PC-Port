#pragma once

#include <optional>
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
 * slid a coordinate and how much it has stretched it. */
struct TextureMotion {
    u32 slot = 0;
    const Texture* frame = nullptr; ///< null for a scroll
    Vec2 offset{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
};

/** Where a keyed scroll stands: how far along it has slid, and the stretch of the
 * coordinate it slides (nought before it starts, which hides what it is on). */
struct ScrollState {
    f32 along = 0.0f;
    f32 scale = 1.0f;
};

/**
 * A level's texture animations as the original steps them once a game frame: a cycle shows
 * the next of its frames every `rate` frames, and a scroll slides the coordinates a step of
 * its cycle each frame. The frames of a cycle are the set's own entries after its source,
 * or entries found by name in the level's set or the sets other archives lend it. Those
 * keyed to a sequence's frame instead (a texture node's, or a sequence's own) do not step:
 * they are read off at a frame, the cycle counting from its first frame at its rate, the
 * scroll easing in over its rate and running to its end.
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
    /** Whether an entry is keyed to a sequence's frame rather than stepped. */
    bool keyed(usize index) const { return m_entries[index].keyed; }

    /** Where an animation stands. */
    TextureMotion motion(usize index) const;
    /** Where the animation numbered `info` in what was bound stands at sequence frame
     * `frame`; nothing when it was not bound. */
    std::optional<TextureMotion> motionAt(s32 info, s32 frame) const;
    /** How far a keyed scroll has run at `sinceStart` frames past its first: the original's
     * easing over `rate` frames, then steady to its `frames`, then held. */
    static f32 scrollAt(s32 sinceStart, s32 rate, s32 frames);
    /** The same with the stretch it puts on the coordinate: none (the picture collapsed)
     * before it starts, then growing from one to `frames / rate` as it runs. */
    static ScrollState scrollStateAt(s32 sinceStart, s32 rate, s32 frames);
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
        bool keyed = false;                 ///< read at a sequence frame, never stepped
        s32 offset = 0;                     ///< a keyed one's first sequence frame
    };

    static void show(const Entry& entry, WorldScene& scene);

    std::vector<Entry> m_entries;
    std::vector<s32> m_entryOfInfo; ///< per animation bound, its entry or -1
    u32 m_frame = 0;
};

} // namespace gdl
