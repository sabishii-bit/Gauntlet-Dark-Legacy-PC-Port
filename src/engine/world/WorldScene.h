#pragma once

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SceneGeometry.h"
#include "engine/world/SelectionHighlight.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

namespace gdl {
class NativeStateDigest;
class WorldCollision;

/**
 * A level's geometry placed by its layout. What never moves is gathered per texture into a
 * few batches, lit once when the scene is built. Objects the layout animates, and those it
 * marks for sorting, stay units of their own: they are placed and lit every frame. Which
 * pass anything draws in follows the authored flags alone (ObjectMaterial): a sortable
 * object is deferred, farthest first, once it is faded, a glow, flagged behind or wears an
 * alpha-textured mesh; everything else, alpha texture or not, is a solid drawn with the
 * cutout test. Textures a texture animation cycles or slides are swapped per slot.
 * One authored word is not followed to the letter: a flat object flagged to skip the depth
 * compare (the Armory's weapon-rack overlay) is tested after all, lifted a little toward
 * the eye so it still lies over the face it was cut from, because the original paints it
 * through the walls in front of it; the camera-facing glows flagged so keep their word.
 * Nor is the order: a solid wearing a translucent texture (the Trenches' haze) is a veil,
 * drawn after the figures that stand under it so it blends over them, where the original,
 * drawing its world before its items, cut them off behind the haze.
 */
class WorldScene {
public:
    static constexpr f32 kAlphaTest = DrawState::kTranslucentAlphaTest;
    /** What additive geometry is shaded: glows, flames and force fields add their whole
     * texture, whichever way they face. */
    static constexpr Color kUnlit{255, 255, 255, 255};
    /** What a vertex of level geometry is shaded. The original never lights it by the
     * level's light (every world node carries ZATR_NOSHADING): additive parts add their
     * whole texture, a mesh with a colour stream (prelit, lightmapped) is its baked colour
     * and anything else is flat (sDrawGeom, prim 128 under the x2 combiner). */
    static Color shadeOf(bool additive, bool prelit, const MeshVertex& vertex);
    /** The same at `position`, with the point lights added and the level's darkening taken. */
    static Color shadeAt(bool additive, bool prelit, const MeshVertex& vertex, const Vec3& position,
                         const Vec3& normal, const WorldLighting& lighting);
    /** A baked five-bit colour at full strength: sDrawGeom divides the vertex colour by 248,
     * so a channel of 248 is the whole texture. */
    static Color bakedShade(Color color);
    /** Negative depth biases defer flagged overlays until after ordinary sorted objects. */
    static constexpr f32 kSortBackBias = -10000.0f;
    static constexpr f32 kSortBehindBias = -20000.0f;
    /** How far an overlay authored without a depth compare is drawn toward the eye. */
    static constexpr f32 kOverlayLift = 0.1f;

    /** Gathers every placed object that has a mesh; false when nothing could be placed.
     * Textures the level's set marks external are looked up by name in `lenders`.
     * `controlledObjects` stay individual units even without animation, so gameplay
     * may hide them without affecting other geometry sharing the same texture.
     * `backgroundObjects` explicitly selects backdrop sheets drawn before solids;
     * the model's sorting flags alone do not make geometry a backdrop.
     * `cameraFadeObjects` opts selected geometry into the short camera-depth fade
     * without changing the authored layout, collision or gameplay opacity. `floors`, when
     * given, tells a decal lying on the level's floor from a veil hanging over it. An object
     * wearing one of `thermal`'s surface textures stands as a unit of its own and adds heat. */
    bool build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
               RenderDevice& device, const WorldLighting& lighting = {},
               std::span<TextureSet* const> lenders = {},
               std::span<const usize> controlledObjects = {},
               std::span<const usize> backgroundObjects = {},
               std::span<const usize> cameraFadeObjects = {},
               const WorldCollision* floors = nullptr, const ThermalMaterial& thermal = {});
    /** A sheet no thicker than this, lying on another object's floor, is a decal. */
    static constexpr f32 kDecalThickness = 1.0f;
    static constexpr f32 kDecalLift = 0.5f; ///< how far above its floor a decal may lie

    void clear();
    bool built() const { return !m_batches.empty() || !m_units.empty(); }
    usize placedCount() const { return m_placed; }
    usize batchCount() const { return m_batches.size(); }
    usize unitCount() const { return m_units.size(); }
    usize triangleCount() const { return m_triangles; }
    /** Whether an object is placed every frame: it, or something above it, is animated. */
    bool moving(usize object) const;
    /** Marks `object` out with `highlight` where it stands now, as the developer console's
     * selection; nothing for one that has no mesh. */
    void drawHighlight(RenderDevice& device, const Mat4& clip, usize object,
                       const SelectionHighlight& highlight) const;

    /** Moves an animated object: `local` replaces its offset from its parent, and everything
     * under it follows. Objects that are not animated stay where the layout put them. */
    void setObjectTransform(usize object, const Mat4& local, bool presentationCut = false);
    /** Current tick transform relative to the parent, without render interpolation. */
    const Mat4& localTransform(usize object) const;
    /** Captures the current local transforms before one fixed simulation update.
     * Drawing may interpolate these; collision always reads the current transforms. */
    void capturePresentation();
    /** Shows `texture` wherever the level's texture `slot` is drawn; null restores it. */
    void setTextureFrame(u32 slot, const Texture* texture);
    /** Soft-sprite cycle sampled at draw time; opaque materials retain the native frame. */
    void setTextureCycle(u32 slot, std::span<const Texture* const> frames, f32 position, f32 speed);
    /** Slides the coordinates of everything drawn with `slot`. */
    void setTextureOffset(u32 slot, const Vec2& offset);
    /** Continuous scroll sampled relative to the current whole texture frame. Native
     * offset queries remain unchanged; phase includes each axis's rate-divider remainder. */
    void setTextureScroll(u32 slot, const Vec2& offset, const Vec2& phase, const Vec2& velocity);
    /** Whole authored texture frames elapsed; heat follows this clock plus the same
     * fractional draw sample as animated materials, never wall-clock time. */
    void setTextureTime(u32 frame) { m_textureFrame = frame; }
    /** The texture drawn for a slot, or null when the scene never draws it. */
    const Texture* textureOf(u32 slot) const;
    Vec2 textureOffset(u32 slot) const;
    /** An object's placement composed with every ancestor's, as it stands now. */
    const Mat4& worldTransform(usize object) const;
    /** Fades an object drawn as a unit: 1 as placed, 0 gone. Others are unchanged. */
    void setObjectAlpha(usize object, f32 alpha);
    f32 objectAlpha(usize object) const;
    /** Changes only this unit's visibility, not its children, alpha or collision.
     * Returns false for baked geometry or an absent object. */
    bool setObjectVisible(usize object, bool visible);
    bool objectVisible(usize object) const;

    SceneGeometry geometry() const;
    bool acceptsGeometry(const SceneGeometry& state) const;
    /** Atomic visual restore on a separately owned renderer. Does not run level
     * events or modify the WorldCollision used by the authoritative simulation. */
    bool applyGeometry(const SceneGeometry& state);

    /** Draws the still solid geometry, the undeferred objects, the deferred objects,
     * then the sorted objects farthest from the camera first (those flagged to face it
     * turned its way), and the glows last; `clip` maps world to clip space.
     * A texture-frame offset samples scrolls and soft flipbooks without advancing clocks. */
    void draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
              f32 presentationAlpha = -1.0f, std::optional<f32> textureFrameOffset = {}) const;
    /** Insert dynamic solid objects between these passes so glass and light rays
     * blend over them while still respecting the completed depth buffer. */
    void drawOpaque(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                    f32 presentationAlpha = -1.0f,
                    std::optional<f32> textureFrameOffset = {}) const;
    /** The veils: solids wearing a translucent texture, drawn with the solids' depth rules
     * once the figures standing under them have drawn, so they blend over those figures. */
    void drawVeils(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                   f32 presentationAlpha = -1.0f, std::optional<f32> textureFrameOffset = {}) const;
    void drawDeferred(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                      f32 presentationAlpha = -1.0f,
                      std::optional<f32> textureFrameOffset = {}) const;
    /** Takes this much of the colour out of everything but what glows (the level's light is
     * baked into its vertices, so a change of ambient light is made this way). */
    /** The light everything placed is lit by as it is drawn, as the level scales it. */
    void setLighting(const WorldLighting& lighting) {
        m_lighting = lighting;
        m_lighting.darken = m_darken;
    }
    /** How much is taken from the baked colour of the batches and of what is lit here as it
     * is drawn; the point lights add theirs over what is left. */
    void setDarken(f32 darken) {
        m_darken = darken;
        m_lighting.darken = darken;
    }
    /** This frame's point lights, which light everything but what glows. */
    void setPointLights(std::span<const PointLight> points);
    f32 darken() const { return m_darken; }
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& eye = Vec3{0.0f}) const {
        draw(device, clip, CameraFrame::at(eye));
    }

private:
    /** One of the level's texture indices as the scene draws it. */
    struct SlotMotion {
        const Texture* frame = nullptr; ///< what an animation shows instead, when set
        std::vector<const Texture*> cycle;
        f32 cyclePosition = 0.0f;
        f32 cycleSpeed = 0.0f;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scrollPhase{0.0f};
        Vec2 scrollVelocity{0.0f};
        bool operator==(const SlotMotion&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const SlotMotion& value) {
            const auto& [frame, cycle, cyclePosition, cycleSpeed, offset, scrollPhase,
                         scrollVelocity] = value;
            digest.resource(frame);
            digest.wide(cycle.size());
            for (const auto* texture : cycle) {
                digest.resource(texture);
            }
            digest.values(cyclePosition, cycleSpeed, offset, scrollPhase, scrollVelocity);
        }
    };
    struct Slot : SlotMotion {
        const Texture* texture = nullptr; ///< immutable archive binding
        bool usable = false;
        bool thermal = false;

        const Texture* current() const { return frame != nullptr ? frame : texture; }
        Vec2 presentedOffset(std::optional<f32> frameOffset) const;
        const Texture* presentedTexture(DrawState& state, std::optional<f32> frameOffset) const;
    };
    struct Batch {
        u32 slot = 0;
        const Texture* lightmap = nullptr; ///< scales the colour by its alpha, when set
        Vec2 lightmapScale{1.0f, 1.0f};    ///< texels of the lightmap to its [0, 1] range
        ImmediateBatch geometry;
        std::vector<Vec3> normals; ///< one per vertex, for the point lights
        Vec3 lowest{0.0f};         ///< the corners of what it holds
        Vec3 highest{0.0f};
        bool additive = false;
        bool depthWrite = true;
        bool depthTest = true;
        bool veil = false; ///< wears a translucent texture: drawn after the figures
    };
    struct UnitPart {
        u32 slot = 0;
        const MeshPart* part = nullptr;
        const Texture* lightmap = nullptr;
        Vec2 lightmapScale{1.0f, 1.0f};
        bool additive = false;
    };
    /** An object placed every frame: animated, or drawn in depth order. */
    struct UnitState {
        f32 alpha = 1.0f;
        bool visible = true;
        bool operator==(const UnitState&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const UnitState& value) {
            digest.template fields<2>(value);
        }
    };
    struct Unit : UnitState {
        usize object = 0;
        const Mesh* mesh = nullptr;
        std::vector<UnitPart> parts;
        f32 sortBias = 0.0f;
        f32 distanceFadeRange = 0.0f;
        f32 distanceSortScale = 1.0f;
        u32 facing = 0;
        bool prelit = false; ///< shaded by its vertices' colours ///< turned to the camera this way
        bool chrome = false;
        bool sorted = false;      ///< eligible for the deferred queue (flag 0x800)
        bool deferAlways = false; ///< a glow or sort-behind object
        bool meshAlpha = false;   ///< its mesh is marked alpha-textured
        bool background = false;  ///< depthless, far-layer scenery (e.g. A5 lightning sheets)
        bool depthWrite = true;
        bool depthTest = true;
        bool lifted = false; ///< authored without a depth compare: tested, moved toward the eye
        bool veil = false;   ///< a solid wearing a translucent texture: drawn after the figures

        /** Drawn after the solids, farthest first, as the original's MBSetupObject decides. */
        bool deferred(f32 opacity) const {
            return (distanceFadeRange > 0 && opacity < 1) ||
                   (sorted && (opacity < 1.0f || deferAlways || meshAlpha));
        }
    };
    struct PlacementState {
        Mat4 local{1.0f};
        Mat4 previous{1.0f};
        u32 continuity = 1;
        bool operator==(const PlacementState&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const PlacementState& value) {
            digest.template fields<3>(value);
        }
    };
    struct Placement : PlacementState {
        Mat4 initial{1.0f}; ///< immutable authored placement
        s32 parent = -1;
        s32 unit = -1;
        bool moving = false;
    };
    /** A version owns its values and their digest together. History may retain an
     * earlier static pose after a diagnostic restore or presentation settlement. */
    struct StaticPlacements {
        explicit StaticPlacements(std::vector<PlacementState> states = {});
        std::vector<PlacementState> values;
        CanonicalDigestSink::Digest fingerprint{};
    };

    /** Static placement values are shared by native history until a diagnostic
     * restore changes them. Ordinary world updates only copy the moving roster. */
    void captureStaticPlacements();

    Slot& slotFor(u32 index, TextureSet& textures, RenderDevice& device,
                  std::span<TextureSet* const> lenders, const ThermalMaterial& thermal);
    const Mat4& worldOf(usize object) const;
    const Mat4& presentedWorldOf(usize object, f32 alpha) const;
    Unit* unitOf(usize object);
    const Unit* unitOf(usize object) const;
    f32 drawnOpacity(const Unit& unit, const CameraFrame& camera, f32 presentationAlpha) const;
    void drawBatch(RenderDevice& device, const Batch& batch, const Mat4& clip,
                   std::optional<f32> textureFrameOffset) const;
    /** Draws the unit's plain parts, its additive parts, or both. */
    void drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                  const CameraFrame& camera, bool plainParts, bool additiveParts, f32 alpha,
                  std::optional<f32> textureFrameOffset) const;

public:
    class LocalSnapshot {
        friend class WorldScene;
        std::vector<PlacementState> m_placements;
        std::shared_ptr<const StaticPlacements> m_staticPlacements;
        bool m_staticPreviousPending = false;
        std::vector<UnitState> m_units;
        std::vector<std::pair<u32, SlotMotion>> m_slots;
        WorldLighting m_lighting;
        f32 m_darken = 0;
        u32 m_textureFrame = 0;
        const WorldScene* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    /** Native values only: geometry buffers, mesh bindings, and draw scratch
     * stay with the live scene. No resource lookup or GPU work during restore. */
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    /** Exact captured values under this binding; static catalogs compare by value,
     * while derived composition and draw scratch are outside native history. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    const CanonicalDigestSink::Digest& rosterDigest() const;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    std::unordered_map<u32, Slot> m_slots;
    std::vector<const Mesh*> m_objectMeshes; ///< one per layout object, null for none
    std::vector<Batch> m_batches;
    std::vector<Unit> m_units;
    std::vector<Placement> m_placements;
    std::vector<usize> m_movingPlacements;
    std::shared_ptr<const StaticPlacements> m_staticPlacements =
        std::make_shared<const StaticPlacements>();
    mutable std::optional<CanonicalDigestSink::Digest> m_rosterDigest;
    bool m_staticPreviousPending = false;
    WorldLighting m_lighting;
    f32 m_darken = 0.0f;
    u32 m_textureFrame = 0;
    u64 m_layoutSignature = 0;
    usize m_placed = 0;
    usize m_triangles = 0;
    mutable std::vector<Mat4> m_world; ///< per object, composed for the frame being drawn
    mutable std::vector<u8> m_worldValid;
    mutable std::vector<Mat4> m_presentedWorld;
    mutable std::vector<u8> m_presentedValid;
    mutable std::vector<usize> m_order; ///< the sorted units, farthest first
    mutable std::vector<usize> m_chain; ///< ancestors awaiting composition
    mutable ImmediateBatch m_scratch;
    mutable ImmediateBatch m_lit; ///< a batch with this frame's point lights added
};

} // namespace gdl
