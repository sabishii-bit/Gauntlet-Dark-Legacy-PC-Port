#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreeParticles.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

class NativeStateResources;

/** Something solid a level's item puts in the way: a box or an upright cylinder. */
struct Obstacle {
    struct ItemQuery {
        template <class Visitor>
        friend void visitNativeState(Visitor& visitor, const ItemQuery& value) {
            visitor.template fields<2>(value);
        }
        bool cylindrical = false;
        f32 radius = 0;
        bool operator==(const ItemQuery&) const = default;
    };

    Vec3 centre{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    f32 halfAcross = 0.0f; ///< along its own x
    f32 halfAlong = 0.0f;  ///< along its own z
    f32 height = 0.0f;
    bool solid = true;
    f32 cylinderRadius = 0.0f; ///< positive selects a cylinder instead of the box
    // Native cylinder/box items use endpoint/retreat tests for enemy births and
    // route probes. Absent on mesh barriers and synthetic swept-only obstacles.
    std::optional<ItemQuery> enemyItem = std::nullopt;
    bool operator==(const Obstacle&) const = default;
    template <class Visitor> friend void visitNativeState(Visitor& visitor, const Obstacle& value) {
        visitor.template fields<8>(value);
    }

    /** Where a body of `radius` standing at `position` is pushed to so as not to be in the
     * box; `position` itself when it is clear of it, over it or under it. */
    Vec3 pushOut(const Vec3& position, f32 radius) const;
    /** Whether a body of `radius` at `position` is against the box, within `margin`. */
    bool touchedBy(const Vec3& position, f32 radius, f32 margin = 0.3f) const;
    /** Swept line of the given radius, respecting height, shape, yaw and solidity. */
    bool blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const;
    /** First contact fraction on that sweep, or no contact. */
    std::optional<f32> contact(const Vec3& from, const Vec3& to, f32 radius) const;
};

/**
 * One of a level's items as it stands in the world: its figure from the realm's item archive
 * (found by the item's name), playing one of the figure's sequences by its place in the
 * tree, on a loop or once, where the instance puts it on the floor.
 */
class ItemFigure {
public:
    ItemFigure() = default;
    /** Same-process value copy: gameplay poses, clocks and particles are independent;
     * immutable mesh/texture bindings are shared. Archives and device must outlive
     * every copy. No GPU work or cross-process restore protocol is involved. */
    ItemFigure(const ItemFigure& other);
    ItemFigure& operator=(const ItemFigure& other);
    ItemFigure(ItemFigure&&) noexcept = default;
    ItemFigure& operator=(ItemFigure&&) noexcept = default;
    ~ItemFigure() = default;
    /** Same-process native equality under shared loaded artwork. Drawing caches
     * and the resource cache itself are not simulation state. */
    bool equalLocal(const ItemFigure& other) const;
    /** Register retained native bindings, including static fallback trees from
     * discarded branches. Archives must already have logical registry keys. */
    bool registerNativeResources(NativeStateResources& registry) const;
    /** Value equality is local to the same bound artwork, not cross-load equivalence. */
    bool operator==(const ItemFigure& other) const { return equalLocal(other); }

    static constexpr f32 kFloorLift = 0.1f;

    /** Stands the figure `name` of `items` (which must outlive it) where `instance` is, on
     * the floor the collision finds; false when the archive has no such figure, in which
     * case the item still has its place and its obstacle. Explicit texture lenders
     * support decoded native archives; they must outlive the figure. An empty list
     * preserves the ordinary archive-local binding behavior. */
    bool place(RenderDevice& device, ItemArchive& items, std::string_view name,
               const ItemInstance& instance, const WorldCollision* collision,
               std::span<TextureSet* const> lenders = {});
    /** AddItemSub's FloorPos: a radius-one sweep from four above the instance to ten below. */
    static constexpr f32 kFloorReachAbove = 4.0f;
    static constexpr f32 kFloorReachBelow = 10.0f;
    static constexpr f32 kFloorReachRadius = 1.0f;
    /** The same from whichever of the scope's archives holds the tree, in its search order. */
    bool place(RenderDevice& device, const AssetScope& scope, std::string_view name,
               const ItemInstance& instance, const WorldCollision* collision,
               std::span<TextureSet* const> lenders = {});
    /** Scenery may have no animation tree: try its static name with the authored object
     * suffixes across the scope. Ordinary place() callers retain their tree-only contract. */
    bool placeStaticFallback(RenderDevice& device, const AssetScope& scope, std::string_view name,
                             const ItemInstance& instance, const WorldCollision* collision,
                             u32 objectFlags, std::span<TextureSet* const> lenders = {});
    bool placeStaticFallback(RenderDevice& device, ItemArchive& items, std::string_view name,
                             const ItemInstance& instance, const WorldCollision* collision,
                             u32 objectFlags, std::span<TextureSet* const> lenders = {}) {
        return placeStaticFallback(device, AssetScope(items), name, instance, collision,
                                   objectFlags, lenders);
    }

    /** Whether a sequence starts over when it ends or holds its last frame. */
    enum class Playback : u8 { Once, Loop };
    /** Starts the figure's sequence number `index`, played as `playback` says. */
    void play(s32 index, Playback playback);
    void update(f32 seconds);
    /** Captures visual state before a fixed update, including ticks that hold animation. */
    void capturePresentation();
    /** Per-draw presentation only. Hidden meshes leave child attachments intact;
     * overrides never become part of the shared artwork or gameplay state. */
    struct Appearance {
        Color tint = Color::white();
        bool unlit = false;
        std::span<const usize> hiddenMeshes;
        f32 whiteBlend = 0.0f;
        bool doubleSided = false; ///< explicit native display-list culling override
    };
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 alpha = 1.0f, f32 scale = 1.0f, const CameraFrame* camera = nullptr,
              TreeModel::Pass pass = TreeModel::Pass::All, f32 presentationAlpha = -1.0f,
              const Appearance* appearance = nullptr) const;
    /** Traps suppress emission in OFF without killing their live particle tails. */
    void gateParticlesOnSequence(bool enabled);
    const TreeParticles& particles() const { return m_sim.particles; }
    const TextureAnimator& textureAnimator() const { return textures(); }

    bool hasFigure() const { return m_sim.tree != nullptr; }
    /** The tree the figure plays, or null for a placement with nothing to show. */
    const TreeInfo* tree() const { return m_sim.tree; }
    s32 sequence() const { return m_sim.index; }
    /** Whether a sequence played once has reached its end (a missing one has at once). */
    bool finished() const;
    /** Current posed attachment, including the instance's placement. */
    std::optional<Mat4> nodeTransform(std::string_view name) const;
    f32 progress() const;
    /** How many sequences the figure has. */
    usize sequenceCount() const { return m_sim.tree != nullptr ? m_sim.tree->sequences.size() : 0; }
    /** How long the sequence number `index` lasts, in ticks of a sixtieth. */
    s32 ticksOf(s32 index) const;
    /** The sequence number `index` itself, or null without one. */
    const TreeSequenceInfo* sequenceInfo(s32 index) const;
    const Vec3& position() const { return m_sim.position; }
    f32 yaw() const { return m_sim.yaw; }
    /** How the figure is placed in the world. */
    const Mat4& transform() const { return m_sim.transform; }
    /** Read-only draw cursors. Empty states can retain a different pose and
     * texture cursor from the sequence controlling visible object meshes. */
    struct Presentation {
        const TreeInfo* tree = nullptr;
        const ItemArchive* archive = nullptr;
        u32 poseSequence = 0;
        f32 poseFrame = 0;
        u64 poseGeneration = 0;
        u32 meshSequence = 0;
        f32 meshFrame = 0;
        u32 textureSequence = 0;
        f32 textureFrame = 0;
        f32 textureClock = 0;
        u32 continuity = 1;
        bool drawable = false;
    };
    Presentation presentation() const;
    /** Moves the entire figure with an authored attachment, retaining its animation. */
    void placeAt(const Mat4& placement);
    /** Rocks the figure where it stands: pitched by `pitch` and turned `yaw` off its facing. */
    void tilt(f32 pitch, f32 yaw);
    /** Stands the figure back as it was placed. */
    void rest() { m_sim.transform = m_sim.placement; }
    /** The box the item's record gives it, where the figure stands. */
    Obstacle obstacle(const ItemInfo& info) const;

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const ItemFigure& /*figure*/);
    void refreshTextures();

    struct Resources {
        std::shared_ptr<TreeInfo> staticTree;
        TreeModel model;
        TextureAnimator textures;
    };
    struct Simulation {
        std::shared_ptr<Resources> resources = std::make_shared<Resources>();
        TextureAnimator::Clock textureClock;
        const TreeInfo* tree = nullptr;
        const ItemArchive* archive = nullptr;
        u32 poseSequence = 0;
        f32 poseFrame = 0;
        u64 poseGeneration = 0;
        u32 continuity = 1;
        TreePose pose;
        Mat4 previousTransform{1};
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationCaptured = false;
        bool presentationAdvanced = false;
        AnimationPlayer player;
        mutable TreeParticles particles;
        u32 textureSequence = 0;
        s32 textureFrame = 0;
        bool gateParticles = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 yaw = 0.0f;
        Mat4 transform{1.0f};
        Mat4 placement{1.0f}; ///< the transform `place` gave it
        s32 index = -1;
        bool loop = false;
        bool holdPose = false;
    };
    struct CachedBinding {
        const ItemArchive* archive = nullptr;
        RenderDevice* device = nullptr;
        std::string name;
        bool staticModel = false;
        u32 flags = 0;
        const TreeInfo* tree = nullptr;
        std::shared_ptr<Resources> resources;
        TreeParticles particles;
        std::vector<TextureSet*> lenders;
    };
    // Resource bindings survive correction and are bounded by the distinct artwork
    // used by this figure, not by the number of speculative births.
    using Bindings = std::vector<CachedBinding>;
    std::shared_ptr<Bindings> m_bindings = std::make_shared<Bindings>();
    Simulation m_sim;
    mutable TreePose m_presentationPose;
    TextureAnimator& textures() const;
};

/** Whether a party of `players` sees an item placed for `minPlayers`, by the original's
 * rule: at least that many, or exactly ten less than it when it is over ten. */
bool shownToParty(s32 minPlayers, s32 players);

/** Places authored item angles by world-axis pitch, yaw, then roll, followed by translation. */
Mat4 itemPlacement(const Vec3& position, const Vec3& rotation);

} // namespace gdl::game
