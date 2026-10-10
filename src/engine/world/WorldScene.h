#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SceneGeometry.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

namespace gdl {

/**
 * A level's geometry placed by its layout. What never moves is gathered per texture into a
 * few batches, lit once when the scene is built. Objects the layout animates, and those it
 * marks for sorting, stay units of their own: they are placed and lit every frame and the
 * sorted ones are drawn farthest first, the way the original defers its translucent
 * objects. Textures a texture animation cycles or slides are swapped per slot.
 */
class WorldScene {
public:
    static constexpr f32 kAlphaTest = DrawState::kTranslucentAlphaTest;
    /** What additive geometry is shaded: glows, flames and force fields add their whole
     * texture, whichever way they face. */
    static constexpr Color kUnlit{255, 255, 255, 255};
    /** What a vertex is shaded: additive parts whole, prelit ones by their own colour, the
     * rest by the lights. */
    static Color shadeOf(bool additive, bool prelit, const MeshVertex& vertex, const Vec3& normal,
                         const WorldLighting& lighting);
    /** The same at `position`, with the point lights added. */
    static Color shadeAt(bool additive, bool prelit, const MeshVertex& vertex, const Vec3& position,
                         const Vec3& normal, const WorldLighting& lighting);
    /** Negative depth biases defer flagged overlays until after ordinary sorted objects. */
    static constexpr f32 kSortBackBias = -10000.0f;
    static constexpr f32 kSortBehindBias = -20000.0f;

    /** Gathers every placed object that has a mesh; false when nothing could be placed.
     * Textures the level's set marks external are looked up by name in `lenders`.
     * `controlledObjects` stay individual units even without animation, so gameplay
     * may hide them without affecting other geometry sharing the same texture.
     * `backgroundObjects` explicitly selects backdrop sheets drawn before solids;
     * the model's sorting flags alone do not make geometry a backdrop. */
    bool build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
               RenderDevice& device, const WorldLighting& lighting = {},
               std::span<TextureSet* const> lenders = {},
               std::span<const usize> controlledObjects = {},
               std::span<const usize> backgroundObjects = {});

    void clear();
    bool built() const { return !m_batches.empty() || !m_units.empty(); }
    usize placedCount() const { return m_placed; }
    usize batchCount() const { return m_batches.size(); }
    usize unitCount() const { return m_units.size(); }
    usize triangleCount() const { return m_triangles; }
    /** Whether an object is placed every frame: it, or something above it, is animated. */
    bool moving(usize object) const;

    /** Moves an animated object: `local` replaces its offset from its parent, and everything
     * under it follows. Objects that are not animated stay where the layout put them. */
    void setObjectTransform(usize object, const Mat4& local, bool presentationCut = false);
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

    /** Draws the still opaque geometry, the moving objects, the still translucent geometry,
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
    void drawDeferred(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                      f32 presentationAlpha = -1.0f,
                      std::optional<f32> textureFrameOffset = {}) const;
    /** Takes this much of the colour out of everything but what glows (the level's light is
     * baked into its vertices, so a change of ambient light is made this way). */
    void setDarken(f32 darken) { m_darken = darken; }
    /** This frame's point lights, which light everything but what glows. */
    void setPointLights(std::span<const PointLight> points);
    f32 darken() const { return m_darken; }
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& eye = Vec3{0.0f}) const {
        draw(device, clip, CameraFrame::at(eye));
    }

private:
    /** One of the level's texture indices as the scene draws it. */
    struct Slot {
        const Texture* texture = nullptr; ///< what the set (or a lender) holds for it
        const Texture* frame = nullptr;   ///< what an animation shows instead, when set
        std::vector<const Texture*> cycle;
        f32 cyclePosition = 0.0f;
        f32 cycleSpeed = 0.0f;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scrollPhase{0.0f};
        Vec2 scrollVelocity{0.0f};
        bool translucent = false;
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
        bool translucent = false;
        bool additive = false;
        bool depthWrite = true;
        bool depthTest = true;
    };
    struct UnitPart {
        u32 slot = 0;
        const MeshPart* part = nullptr;
        const Texture* lightmap = nullptr;
        Vec2 lightmapScale{1.0f, 1.0f};
        bool translucent = false;
        bool additive = false;
    };
    /** An object placed every frame: animated, or drawn in depth order. */
    struct Unit {
        usize object = 0;
        const Mesh* mesh = nullptr;
        std::vector<UnitPart> parts;
        f32 sortBias = 0.0f;
        f32 alpha = 1.0f;
        u32 facing = 0;
        bool prelit = false; ///< shaded by its vertices' colours ///< turned to the camera this way
        bool chrome = false;
        bool sorted = false;
        bool background = false; ///< depthless, far-layer scenery (e.g. A5 lightning sheets)
        bool visible = true;
        bool depthWrite = true;
        bool depthTest = true;
    };
    struct Placement {
        Mat4 local{1.0f};    ///< relative to the parent
        Mat4 initial{1.0f};  ///< loaded layout, before trigger placement
        Mat4 previous{1.0f}; ///< last fixed-update snapshot, presentation only
        s32 parent = -1;
        s32 unit = -1;
        u32 continuity = 1;
        bool moving = false;
    };

    Slot& slotFor(u32 index, TextureSet& textures, RenderDevice& device,
                  std::span<TextureSet* const> lenders);
    const Mat4& worldOf(usize object) const;
    const Mat4& presentedWorldOf(usize object, f32 alpha) const;
    Unit* unitOf(usize object);
    const Unit* unitOf(usize object) const;
    void drawBatch(RenderDevice& device, const Batch& batch, const Mat4& clip,
                   std::optional<f32> textureFrameOffset) const;
    void drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                  const CameraFrame& camera, bool opaque, bool translucent, f32 alpha,
                  std::optional<f32> textureFrameOffset) const;

    std::unordered_map<u32, Slot> m_slots;
    std::vector<Batch> m_batches;
    std::vector<Unit> m_units;
    std::vector<Placement> m_placements;
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
