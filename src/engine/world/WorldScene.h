#pragma once

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
    /** The sort keys of objects flagged to draw behind the rest, and behind those. */
    static constexpr f32 kSortBackBias = -10000.0f;
    static constexpr f32 kSortBehindBias = -20000.0f;

    /** Gathers every placed object that has a mesh; false when nothing could be placed.
     * Textures the level's set marks external are looked up by name in `lenders`. */
    bool build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
               RenderDevice& device, const WorldLighting& lighting = {},
               std::span<TextureSet* const> lenders = {});

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
    void setObjectTransform(usize object, const Mat4& local);
    /** Shows `texture` wherever the level's texture `slot` is drawn; null restores it. */
    void setTextureFrame(u32 slot, const Texture* texture);
    /** Slides the coordinates of everything drawn with `slot`. */
    void setTextureOffset(u32 slot, const Vec2& offset);
    /** The texture drawn for a slot, or null when the scene never draws it. */
    const Texture* textureOf(u32 slot) const;
    Vec2 textureOffset(u32 slot) const;
    /** An object's placement composed with every ancestor's, as it stands now. */
    const Mat4& worldTransform(usize object) const;
    /** Fades an object drawn as a unit: 1 as placed, 0 gone. Others are unchanged. */
    void setObjectAlpha(usize object, f32 alpha);
    f32 objectAlpha(usize object) const;

    /** Draws the still opaque geometry, the moving objects, the still translucent geometry,
     * then the sorted objects farthest from the camera first (those flagged to face it
     * turned its way), and the glows last; `clip` maps world to clip space. */
    void draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera) const;
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& eye = Vec3{0.0f}) const {
        draw(device, clip, CameraFrame::at(eye));
    }

private:
    /** One of the level's texture indices as the scene draws it. */
    struct Slot {
        const Texture* texture = nullptr; ///< what the set (or a lender) holds for it
        const Texture* frame = nullptr;   ///< what an animation shows instead, when set
        Vec2 offset{0.0f, 0.0f};
        bool translucent = false;
        bool usable = false;

        const Texture* current() const { return frame != nullptr ? frame : texture; }
    };
    struct Batch {
        u32 slot = 0;
        const Texture* lightmap = nullptr; ///< scales the colour by its alpha, when set
        Vec2 lightmapScale{1.0f, 1.0f};    ///< texels of the lightmap to its [0, 1] range
        ImmediateBatch geometry;
        bool translucent = false;
        bool additive = false;
        bool depthWrite = true;
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
        bool depthWrite = true;
    };
    struct Placement {
        Mat4 local{1.0f}; ///< relative to the parent
        s32 parent = -1;
        bool moving = false;
    };

    Slot& slotFor(u32 index, TextureSet& textures, RenderDevice& device,
                  std::span<TextureSet* const> lenders);
    const Mat4& worldOf(usize object) const;
    Unit* unitOf(usize object);
    const Unit* unitOf(usize object) const;
    void drawBatch(RenderDevice& device, const Batch& batch, const Mat4& clip) const;
    void drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                  const CameraFrame& camera, bool opaque, bool translucent) const;

    std::unordered_map<u32, Slot> m_slots;
    std::vector<Batch> m_batches;
    std::vector<Unit> m_units;
    std::vector<Placement> m_placements;
    WorldLighting m_lighting;
    usize m_placed = 0;
    usize m_triangles = 0;
    mutable std::vector<Mat4> m_world; ///< per object, composed for the frame being drawn
    mutable std::vector<u8> m_worldValid;
    mutable std::vector<usize> m_order; ///< the sorted units, farthest first
    mutable std::vector<usize> m_chain; ///< ancestors awaiting composition
    mutable ImmediateBatch m_scratch;
};

} // namespace gdl
