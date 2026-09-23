#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreeParticles.h"
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
    static constexpr float kStillSeconds = 1.0f; ///< how long a tree without a sequence shows

    /** How an effect is set going, beyond where. */
    struct Setting {
        float scale = 1.0f;
        float yaw = 0.0f;                ///< turned about the upright
        Vec3 velocity{0.0f, 0.0f, 0.0f}; ///< carried along, as what a move sends flying is
        float seconds = 0.0f; ///< over nought, it repeats for this long instead of playing once
        bool loop = true;     ///< with seconds, false holds the final pose until the lifetime ends
        /** With `seconds`: the tree plays once and this one then repeats in its place. */
        std::string then;
        bool unlit = false;
        bool depthWrite = true;
        Color tint = Color::white();
        float playbackRate = 1.0f; ///< animation speed, independent of motion and particle clocks
    };

    /** One effect playing. */
    struct Effect {
        std::string name;
        unsigned int id = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float scale = 1.0f;
        float yaw = 0.0f;
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        bool repeats = false;
        bool timed = false; ///< lifetime is separate from animation speed or completion
        std::string then;   ///< the tree that takes over once this has played
        RenderDevice* device = nullptr;
        const TreeInfo* tree = nullptr;
        ItemArchive* archive = nullptr;
        TreeModel model;
        TreePose pose;
        AnimationPlayer player;
        float secondsLeft = 0.0f; ///< for a tree without a sequence
        bool unlit = false;
        bool depthWrite = true;
        Color tint = Color::white();
        float playbackRate = 1.0f;
        ParticleField trails;           ///< code-created emitters following the effect's root
        TreeParticles particles;        ///< particle nodes authored in the archive
        std::optional<Mat4> attachment; ///< full posed parent, rather than world yaw alone
        Mat4 transform() const;
    };

    /** Starts `tree` of `archive` (which must outlive the effect) at `position`; false, with
     * a warning, when the archive lacks it. */
    bool start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
               const Vec3& position, float scale = 1.0f);
    /** The same, turned, moving or repeating as `setting` says; its number, or nought. */
    unsigned int startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                          const Vec3& position, const Setting& setting);
    /** Ends effect number `id` now. */
    void stop(unsigned int id);
    /** Puts effect number `id` at `position`, as one that goes about with a character. */
    void moveTo(unsigned int id, const Vec3& position);
    /** Places an effect on a fully posed attachment. Its own scale still applies. */
    void placeAt(unsigned int id, const Mat4& attachment);
    /** Attaches an emitter to the effect root; existing particles remain in world space
     * unless its descriptor explicitly requests dynamic particles. */
    void attachTrail(unsigned int id, const ParticleDescriptor& descriptor, const Texture& texture);
    bool playing(unsigned int id) const;
    void update(float seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;
    void clear();

    std::size_t count() const { return m_effects.size(); }
    const Effect& effect(std::size_t index) const { return *m_effects[index]; }

private:
    static bool bindVisuals(Effect& effect);
    /** An archive's texture animations, shared by its effects. */
    struct Motion {
        ItemArchive* archive = nullptr;
        TextureAnimator animator;
    };

    std::vector<std::unique_ptr<Effect>> m_effects;
    std::vector<std::unique_ptr<Motion>> m_motions;
    float m_frames = 0.0f;
    unsigned int m_nextId = 1;
};

} // namespace gdl::game
