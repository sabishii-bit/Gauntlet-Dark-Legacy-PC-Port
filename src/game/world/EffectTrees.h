#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/**
 * Effects that are an archive's animation tree played once where something happened, such
 * as a potion's burst of magic: the tree's meshes posed by its first sequence, its object
 * nodes flipping through their frames and its textures animating, at a place and a size,
 * gone when the sequence ends.
 */
class EffectTrees {
public:
    static constexpr f32 kStillSeconds = 1.0f; ///< how long a tree without a sequence shows

    /** How an effect is set going, beyond where. */
    struct Setting {
        f32 scale = 1.0f;
        f32 yaw = 0.0f;                   ///< turned about the upright
        Vec3 velocity{0.0f, 0.0f, 0.0f};  ///< carried along, as what a move sends flying is
        f32 seconds = 0.0f; ///< over nought, it repeats for this long instead of playing once
        /** With `seconds`: the tree plays once and this one then repeats in its place. */
        std::string then;
    };

    /** One effect playing. */
    struct Effect {
        std::string name;
        u32 id = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 scale = 1.0f;
        f32 yaw = 0.0f;
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        bool repeats = false;
        std::string then;          ///< the tree that takes over once this has played
        RenderDevice* device = nullptr;
        const TreeInfo* tree = nullptr;
        ItemArchive* archive = nullptr;
        TreeModel model;
        TreePose pose;
        AnimationPlayer player;
        f32 secondsLeft = 0.0f; ///< for a tree without a sequence
    };

    /** Starts `tree` of `archive` (which must outlive the effect) at `position`; false, with
     * a warning, when the archive lacks it. */
    bool start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
               const Vec3& position, f32 scale = 1.0f);
    /** The same, turned, moving or repeating as `setting` says; its number, or nought. */
    u32 startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                 const Vec3& position, const Setting& setting);
    /** Ends effect number `id` now. */
    void stop(u32 id);
    /** Puts effect number `id` at `position`, as one that goes about with a character. */
    void moveTo(u32 id, const Vec3& position);
    bool playing(u32 id) const;
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();

    usize count() const { return m_effects.size(); }
    const Effect& effect(usize index) const { return *m_effects[index]; }

private:
    /** An archive's texture animations, shared by its effects. */
    struct Motion {
        ItemArchive* archive = nullptr;
        TextureAnimator animator;
    };

    std::vector<std::unique_ptr<Effect>> m_effects;
    std::vector<std::unique_ptr<Motion>> m_motions;
    f32 m_frames = 0.0f;
    u32 m_nextId = 1;
};

} // namespace gdl::game
