#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
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

class NativeStateResources;

/**
 * Effects that are an archive's animation tree played once where something happened, such
 * as a potion's burst of magic: the tree's meshes posed by its first sequence, its object
 * nodes flipping through their frames and its textures animating, at a place and a size,
 * gone when the sequence ends.
 */
class EffectTrees {
public:
    EffectTrees() = default;
    EffectTrees(const EffectTrees&) = delete;
    EffectTrees& operator=(const EffectTrees&) = delete;
    EffectTrees(EffectTrees&& other) noexcept;
    EffectTrees& operator=(EffectTrees&& other) noexcept;
    ~EffectTrees() = default;
    static constexpr f32 kStillSeconds = 1.0f; ///< how long a tree without a sequence shows

    /** A light an effect gives off while it plays (SfxSetLight). */
    struct Light {
        Vec3 color{1.0f, 1.0f, 1.0f};
        f32 radius = 0.0f;
        /** Whether the radius rises and falls again over the effect's life, as a damage
         * row's does over its window (ProcessEffects' light scale); else it holds. */
        bool swells = false;
        bool operator==(const Light&) const = default;
    };

    /** How an effect is set going, beyond where. */
    struct Setting {
        f32 scale = 1.0f;
        Vec3 stretch{1.0f, 1.0f, 1.0f};  ///< per axis, on top of the scale
        f32 fadeSeconds = 0.0f;          ///< over nought, it fades out over this much of its end
        f32 yaw = 0.0f;                  ///< turned about the upright
        Vec3 velocity{0.0f, 0.0f, 0.0f}; ///< carried along, as what a move sends flying is
        f32 seconds = 0.0f; ///< over nought, it repeats for this long instead of playing once
        bool loop = true;   ///< with seconds, false holds the final pose until the lifetime ends
        bool persistent = false; ///< retained until stop/clear, independently of animation
        bool emitParticles = true;
        /** Retail's AAANULLOBJ fallback for static mesh names absent from an effect archive.
         * Opt-in for verified sparse trees; corrupt meshes/textures still fail. */
        bool missingObjectsAreEmpty = false;
        bool settled = false; ///< begin at the final pose, without playing the entrance
        bool additive = false;
        /** With `seconds`: the tree plays once and this one then repeats in its place. */
        std::string then;
        /** Explicit native morph time, independent of the animation's rounded pose. */
        std::optional<f32> morphIn;
        bool holdForMorph = false; ///< suppress translation during the birth tree
        bool shrinks = false;      ///< ProcessEffects' last 0.2 seconds shrink to nothing
        bool unlit = false;
        bool depthWrite = true;
        Color tint = Color::white();
        f32 alpha = 1.0f;        ///< constant opacity, multiplied by any end-of-life fade
        f32 playbackRate = 1.0f; ///< animation speed, independent of motion and particle clocks
        std::optional<Light> light;
        bool operator==(const Setting&) const = default;
    };

    /** Optional caller-owned native emission identity. The pool never invents
     * caster/action coordinates or replaces them while restoring visual state. */
    struct Provenance {
        s32 player = -1;
        u64 generation = 0;
        u16 occurrence = 0;
        u16 effectOrdinal = 0;
        bool operator==(const Provenance&) const = default;
    };

    /** Trusted load-time bindings for callback-free replay. Models, textures and
     * archives remain borrowed; starting a copy never binds or uploads artwork.
     * A tree uses model/particles, a standalone emitter uses descriptor/texture.
     * The texture animator is the freshly bound bank prototype, shared by every
     * copied effect with the same archive and ordered lenders. */
    struct Prepared {
        ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        const TreeModel* model = nullptr;
        const TreeParticles* particles = nullptr;
        const TextureAnimator* textures = nullptr;
        std::span<TextureSet* const> lenders;
        const ParticleDescriptor* descriptor = nullptr;
        const Texture* texture = nullptr;
        std::optional<u32> textureSlot;
        /** Borrowed native binding identity only. Prepared starts never invoke
         * the device. */
        RenderDevice* device = nullptr;
    };

    struct Effect;
    /** Native values copied by local rewind. Artwork stays shared; its mutable
     * draw cache is derived from these values immediately before drawing. */
    struct EffectState {
        u64 instance = 0;
        u32 continuity = 1;
        std::string name;
        u32 id = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 scale = 1.0f;
        Vec3 stretch{1.0f, 1.0f, 1.0f};
        f32 fadeSeconds = 0.0f;
        f32 yaw = 0.0f;
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        bool repeats = false;
        bool timed = false; ///< lifetime is separate from animation speed or completion
        bool persistent = false;
        bool emitParticles = true;
        bool missingObjectsAreEmpty = false;
        bool additive = false;
        std::string then; ///< the tree that takes over once this has played
        std::optional<f32> morphIn;
        bool holdForMorph = false;
        bool shrinks = false;
        bool retiring = false; ///< mesh gone; no new emission, but live particles finish
        RenderDevice* device = nullptr;
        const TreeInfo* tree = nullptr;
        ItemArchive* archive = nullptr;
        std::vector<TextureSet*> lenders; ///< borrowed; same lifetime contract as archive
        std::shared_ptr<TreeModel> model;
        TreePose pose;
        AnimationPlayer player;
        f32 secondsLeft = 0.0f; ///< for a tree without a sequence
        bool unlit = false;
        bool depthWrite = true;
        Color tint = Color::white();
        f32 alpha = 1.0f;
        f32 playbackRate = 1.0f;
        std::optional<Light> light;
        f32 lived = 0.0f;                    ///< seconds since it started
        ParticleField trails;                ///< code-created emitters following the effect's root
        TreeParticles particles;             ///< particle nodes authored in the archive
        std::optional<Mat4> attachment;      ///< full posed parent, rather than world yaw alone
        std::optional<Vec3> flightDirection; ///< arrow effects follow 3D travel, not yaw alone
        std::optional<u32> particleTextureSlot; ///< standalone emitter's animated sprite slot
        Mat4 previousTransform{1};
        std::optional<Vec3> previousDirection;
        const TreeInfo* previousTree = nullptr;
        u64 previousGeneration = 0;
        f32 previousFrame = 0;
        bool presentationCaptured = false;
        std::optional<Provenance> provenance;
        /** Prevalidated future topology for a restored chain. Artwork remains
         * borrowed from the immutable pool catalog; CPU birth state is owned.
         * Native starts leave this empty and use their original bind path. */
        std::shared_ptr<const Effect> morphPrototype;
        Mat4 transform(const Mat4* presentedParent = nullptr) const;
        f32 opacity() const;
    };

    /** One effect playing, with a disposable interpolated pose outside history. */
    struct Effect : EffectState {
        mutable TreePose presentationPose;
    };

    /** Starts `tree` of `archive` (which must outlive the effect) at `position`; false, with
     * a warning, when the archive lacks it. */
    bool start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
               const Vec3& position, f32 scale = 1.0f);
    /** The same, turned, moving or repeating as `setting` says; its number, or nought. */
    u32 startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                 const Vec3& position, const Setting& setting,
                 std::span<TextureSet* const> textureLenders = {});
    /** Same native initialization using already-bound resources. Morph topology
     * is unsupported. Standalone emitters require their actual native birth seed;
     * no local handle or guessed network identity substitutes for that seed. */
    u32 startPrepared(const Prepared& prepared, const Vec3& position, const Setting& setting,
                      const Mat4& particleAttachment = Mat4{1},
                      std::optional<u32> particleSeed = std::nullopt);
    /** A prebound standalone emitter with the native upcoming effect-ID seed. */
    u32 startPreparedParticles(const Prepared& prepared, const Mat4& attachment);
    /** Retains a shared prebound clock even when no effect currently uses it. */
    bool prepareBank(const Prepared& prepared);
    /** Ends effect number `id` now. */
    void stop(u32 id);
    /** Removes the mesh and ends births, retaining particle tails until they expire. */
    void finish(u32 id);
    /** Bind once; the same value is idempotent. Missing effects or conflicting
     * tags are rejected without changing the visual or its original ownership. */
    bool tag(u32 id, const Provenance& provenance);
    /** Ordered fallback texture sets. Set before starting effects; owners must outlive clear(). */
    void setTextureLenders(std::span<TextureSet* const> lenders);
    std::span<TextureSet* const> textureLenders() const { return m_lenders; }
    /** Removes time from the remaining lifetime and caps it, without restarting animation. */
    void shortenLifetime(u32 id, f32 secondsLost, f32 maximum);
    /** Puts effect number `id` at `position`, as one that goes about with a character. */
    void moveTo(u32 id, const Vec3& position);
    /** Redirect a reflected projectile without restarting its animation or particles. */
    void redirect(u32 id, const Vec3& position, const Vec3& velocity);
    /** Places an effect on a fully posed attachment. Its own scale still applies. */
    void placeAt(u32 id, const Mat4& attachment,
                 std::optional<Vec3> flightDirection = std::nullopt);
    /** Attaches an emitter to the effect root; existing particles remain in world space
     * unless its descriptor explicitly requests dynamic particles. */
    void attachTrail(u32 id, const ParticleDescriptor& descriptor, const Texture& texture);
    /** Starts a standalone emitter; its phase and live particles own its lifetime.
     * The texture must outlive this effect. */
    u32 startParticles(RenderDevice& device, ItemArchive& archive,
                       const ParticleDescriptor& descriptor, u32 textureSlot,
                       const Mat4& attachment);
    bool playing(u32 id) const;
    /** Seconds before effect number `id` is over; nullopt when it is not playing. */
    std::optional<f32> remaining(u32 id) const;
    /** The lights the playing effects give off, a unit over each, the newest first. */
    void lights(std::vector<PointLight>& out) const;
    /** Snapshot before a simulation step, including before callers move attached effects. */
    void capturePresentation();
    /** A discontinuous placement or bounce must not blend across its contact point. */
    void snapPresentation(u32 id);
    void update(f32 seconds);
    /** A caller may present a mesh attached to an interpolated actor hierarchy.
     * Borrowed only for this draw; neither gameplay anchors nor particle state
     * are changed. Absent entries keep ordinary effect interpolation. */
    struct DrawAttachment {
        u32 effect = 0;
        Mat4 parent{1};
    };
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr, f32 frameBlend = -1.0f,
              std::span<const DrawAttachment> attachments = {}) const;
    /** End the current effects without discarding this stage's reusable artwork. */
    void reset();
    void clear();

    usize count() const { return m_effects.size(); }
    const Effect& effect(usize index) const { return *m_effects[index]; }
    f32 textureFrame(const Effect& effect) const;
    /** Read-only native bank; copy it before retaining a render-only picture. */
    const TextureAnimator* textureAnimator(const ItemArchive* archive,
                                           std::span<TextureSet* const> lenders) const;
    bool presentationAdvanced() const { return m_presentationAdvanced; }
    /** Fractional clock used by a future startSet with these explicit lenders, followed by
     * this pool's fallback lenders. Empty means that bank has never started;
     * querying does not bind artwork or create/advance a clock. */
    std::optional<f32> textureFrame(const ItemArchive& archive,
                                    std::span<TextureSet* const> textureLenders = {}) const;

private:
    static void initializeTree(Effect& value, const Setting& setting);
    static void initializeParticles(Effect& effect, const ParticleDescriptor& descriptor,
                                    const Texture& texture, u32 textureSlot, const Mat4& attachment,
                                    u32 seed);
    bool bindVisuals(Effect& effect);
    static f32 secondsLeftOf(const EffectState& effect);
    /** An archive's texture animations, shared by its effects. */
    struct Motion {
        ItemArchive* archive = nullptr;
        std::vector<TextureSet*> lenders;
        TextureAnimator animator;
    };

    struct Visual {
        RenderDevice* device = nullptr;
        ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::vector<TextureSet*> lenders;
        bool missingObjectsAreEmpty = false;
        bool emitParticles = false;
        std::shared_ptr<TreeModel> model;
        TreeParticles particles;
    };

public:
    class LocalSnapshot {
    private:
        friend class EffectTrees;
        std::vector<EffectState> m_effects;
        std::vector<Motion*> m_motions;
        std::vector<TextureAnimator::Clock> m_clocks;
        std::vector<TextureSet*> m_lenders;
        bool m_presentationAdvanced = false;
        u32 m_nextId = 1;
        u64 m_nextInstance = 1;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    /** Same-owner, same-resource-epoch in-memory history. No wire validation,
     * resource rebinding, or animation seeking is involved. Archives and lenders
     * must outlive this pool; clear it before unloading their resources. */
    void saveLocal(LocalSnapshot& state) const;
    bool canRestoreLocal(const LocalSnapshot& state) const;
    bool restoreLocal(const LocalSnapshot& state);
    /** Complete native values, with semantic borrowed-resource bindings. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& state) const;
    /** Archives/lender banks must already have stable keys in the registry. */
    bool registerNativeResources(NativeStateResources& registry) const;

private:
    static void appendMotionDigest(NativeStateDigest& digest, const Motion& motion,
                                   const TextureAnimator::Clock& clock);
    Motion& nativeBank(RenderDevice& device, ItemArchive& archive,
                       std::span<TextureSet* const> lenders);

    std::vector<std::unique_ptr<Effect>> m_effects;
    std::vector<Motion*> m_motions;
    std::vector<std::unique_ptr<Motion>> m_motionCache;
    std::vector<Visual> m_visualCache;
    std::vector<TextureSet*> m_lenders;
    bool m_presentationAdvanced = false;
    u32 m_nextId = 1;
    u64 m_nextInstance = 1; ///< independent of resettable local effect handles
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
};

} // namespace gdl::game
