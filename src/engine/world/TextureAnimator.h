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

class TreeModel;
class TreeParticles;

/** What an animation shows now: the frame its cycle has reached, or how far its scroll has
 * slid a coordinate and how much it has stretched it. */
struct TextureMotion {
    u32 slot = 0;
    const Texture* frame = nullptr; ///< null for a scroll
    const Texture* nextFrame = nullptr;
    f32 frameBlend = 0.0f;
    Vec2 offset{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
    std::optional<f32> alpha; ///< subtree opacity for a keyed fade, not a texture change
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
    /** Authored scrolling axis, including its sign (zero for a texture cycle). */
    Vec2 scrollDirection(usize index) const { return m_entries[index].direction; }
    /** Whether an entry is keyed to a sequence's frame rather than stepped. */
    bool keyed(usize index) const { return m_entries[index].keyed; }

    /** Where an animation stands. */
    TextureMotion motion(usize index, std::optional<f32> frameOffset = {}) const;
    /** A preloaded cycle frame, without advancing or clamping an invalid identity. */
    const Texture* cycleFrame(usize index, usize frame) const;
    /** Where the animation numbered `info` in what was bound stands at sequence frame
     * `frame`; nothing when it was not bound. */
    std::optional<TextureMotion> motionAt(s32 info, f32 frame) const;
    std::optional<TextureMotion> motionAt(s32 info, s32 frame) const {
        return motionAt(info, static_cast<f32>(frame));
    }
    /** How far a keyed scroll has run at `sinceStart` frames past its first: the original's
     * easing over `rate` frames, then steady to its `frames`, then held. */
    static f32 scrollAt(s32 sinceStart, s32 rate, s32 frames);
    /** The same with the stretch it puts on the coordinate: none (the picture collapsed)
     * before it starts, then growing from one to `frames / rate` as it runs. */
    static ScrollState scrollStateAt(f32 sinceStart, s32 rate, s32 frames);
    static ScrollState scrollStateAt(s32 sinceStart, s32 rate, s32 frames) {
        return scrollStateAt(static_cast<f32>(sinceStart), rate, frames);
    }
    /** Advances the fixed simulation clock, retaining its fractional remainder for drawing. */
    void advance(f32 seconds);
    /** Sample between the previous and current update; negative alpha requests native steps. */
    std::optional<f32> presentationOffset(f32 alpha) const;
    /** Advances `ticks` game frames. */
    void step(u32 ticks = 1);
    /** Shows every animation where it stands. */
    void apply(WorldScene& scene) const;
    /** Static meshes have clock-driven textures, but no sequence or node overrides. */
    void apply(TreeModel& model, std::optional<f32> frameOffset = {}) const;
    /** Resets a shared model, then applies clock, sequence and texture-node overrides in
     * that order. Sequence overrides must not leak into the next instance's draw. */
    void apply(TreeModel& model, const TreeInfo& tree, u32 sequence, f32 frame,
               std::optional<f32> frameOffset = {}) const;
    void apply(TreeModel& model, const TreeInfo& tree, u32 sequence, s32 frame) const {
        apply(model, tree, sequence, static_cast<f32>(frame));
    }
    /** Resolves particle sprite frames using the same clock and keyed sequence. */
    void apply(TreeParticles& particles, const TreeInfo& tree, u32 sequence, f32 frame,
               std::optional<f32> frameOffset = {}) const;
    void apply(TreeParticles& particles, const TreeInfo& tree, u32 sequence, s32 frame) const {
        apply(particles, tree, sequence, static_cast<f32>(frame));
    }
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
        bool keyed = false; ///< read at a sequence frame, never stepped
        s32 offset = 0;     ///< a keyed one's first sequence frame
        s32 fade = 0;       ///< -1 fades out, +1 fades in, 0 cycles or scrolls
    };

    static void show(const Entry& entry, WorldScene& scene);

    std::vector<Entry> m_entries;
    std::vector<s32> m_entryOfInfo; ///< per animation bound, its entry or -1
    u32 m_frame = 0;
    f32 m_remainder = 0.0f;
    f32 m_advance = 0.0f;
};

} // namespace gdl
