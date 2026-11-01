#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SelectionHighlight.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

namespace gdl {

/**
 * An animation tree standing in the world: each node's mesh at the node's rest offset or
 * wherever a pose's matrices put it, lit like the level and drawn wherever a model matrix
 * places the whole figure. An object node shows whichever of its run of meshes the frame
 * calls for, once told the frame, and nothing before that. A node named DUMMY is the
 * figure's marker (a triangle at its feet pointing its way) and is never drawn.
 */
class TreeModel {
public:
    /** DepthWriting/Effects preserve the actor split. Opaque/Blended additionally defer
     * blended submeshes even when their native material writes depth (pickup shadows).
     * Blended splits in two for a pickup standing among scenery: BlendedAhead is its
     * deferred parts with an ordinary sort key, drawn before the scenery's own deferred
     * queue; BlendedBehind those the authored flags sort behind (a gem's halo, its crystal),
     * which the original's lowered key puts after every ordinary sorted object. */
    enum class Pass : u8 { All, DepthWriting, Effects, Opaque, Blended, BlendedAhead, BlendedBehind };
    /** Whether a pass composites the figure's particles and bursts: once, with its blended
     * parts, never again with the parts sorted behind. */
    static constexpr bool compositesEffects(Pass pass) {
        return pass != Pass::DepthWriting && pass != Pass::Opaque && pass != Pass::BlendedBehind;
    }
    /** World-bound shots must test the already-rendered solid scene, including
     * halo nodes whose authored flags otherwise disable the depth test. */
    enum class Occlusion : u8 { Authored, SolidWorld };
    /** Hit flashes retain the base silhouette; animated death skins also dissolve it. */
    enum class SkinAlpha : u8 { PreserveCoverage, Dissolve };
    /** Gathers meshes and textures, resolving external slots through named lenders.
     * The mesh and texture owners must outlive this model. Missing resources fail the bind. */
    bool bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures, RenderDevice& device,
              std::span<TextureSet* const> lenders = {});

    void clear() {
        m_nodes.clear();
        m_textureSampling.clear();
        m_highlight = nullptr;
        resetTextures();
        setAppearance(false);
        m_whiteBlend = 0;
        m_cullBack = true;
    }
    bool bound() const { return !m_nodes.empty(); }
    usize nodeCount() const { return m_nodes.size(); }

    /** The rest pose's extent in model space. */
    const Vec3& minBounds() const { return m_min; }
    const Vec3& maxBounds() const { return m_max; }

    /** Shows `frame` wherever the parts use texture `slot` of the set (null: the set's own),
     * or slides their coordinates by `offset`, the way texture animations move. */
    void setTextureFrame(u32 slot, const Texture* frame, const Texture* next = nullptr,
                         f32 blend = 0.0f);
    void setTextureOffset(u32 slot, const Vec2& offset, const Vec2& scale = Vec2{1.0f, 1.0f});
    /** Material sampling overrides, retained across texture animation resets. */
    void setTextureSampling(u32 slot, bool clamp, bool mipmaps);
    /** Texture-node changes apply only to that node's subtree. UV transforms affect
     * every material there, while frame substitutions still name a texture slot. */
    void setNodeTextureFrame(usize root, u32 slot, const Texture* frame,
                             const Texture* next = nullptr, f32 blend = 0.0f);
    void setNodeTextureOffset(usize root, const Vec2& offset, const Vec2& scale);
    /** Sets opacity throughout one subtree; resetTextures restores opaque nodes. */
    void setNodeAlpha(usize root, f32 alpha);
    /** How solid tree node `node`'s own mesh draws, leaving what hangs from it alone. */
    void setMeshAlpha(usize node, f32 alpha);
    /** White hit flashes may affect one mesh or an independently animated subtree. */
    void setMeshMaskedTexture(usize index, const Texture* texture);
    void setNodeMaskedTexture(usize root, const Texture* texture);
    void resetTextures();
    /** Alternate skins always retain the base cutouts. Dissolve additionally uses the
     * alternate texture's alpha instead of treating it as solid coverage. */
    void setMaskedTexture(const Texture* texture, SkinAlpha alpha = SkinAlpha::PreserveCoverage) {
        m_maskedTexture = texture;
        m_skinAlpha = alpha;
    }
    /** Full ambient illumination and an optional RGB tint applied to every node. */
    void setAppearance(bool unlit, Color tint = Color::white(), bool depthWrite = true,
                       bool additive = false) {
        m_unlit = unlit;
        m_tint = tint;
        m_depthWrite = depthWrite;
        m_additive = additive;
    }
    /** Keeps writing depth while faded by the draw's alpha, as the original's unseen body
     * does with Z update left on, so sorted scenery drawn after it stays behind it. A
     * dissolving skin still defers. */
    void setFadedDepthWrite(bool keep) { m_fadedDepthWrite = keep; }
    /** Draws both faces of every part: a flat decal seen from either side. */
    void setDoubleSided(bool doubleSided) { m_cullBack = !doubleSided; }
    /** Marks every part out with `highlight` as it is drawn, as the developer console's
     * selection; null marks nothing. */
    void setHighlight(const SelectionHighlight* highlight) { m_highlight = highlight; }
    void setWhiteBlend(f32 fraction) { m_whiteBlend = std::clamp(fraction, 0.0f, 1.0f); }
    const SelectionHighlight* highlight() const { return m_highlight; }
    Vec2 textureOffset(u32 slot) const;
    /** How a slot's coordinates are stretched, one and one when they are not. */
    Vec2 textureScale(u32 slot) const;

    /** Shows the object nodes' meshes for `frame` of `sequence`: the run's mesh for the
     * frame, the only mesh of a one-frame run, else none. */
    void setFrame(u32 sequence, s32 frame);
    /** Opt-in visual mesh morphing for authored vertex-animation runs. Incompatible
     * adjacent topology/material/UV frames remain discrete. */
    void setPresentationFrame(u32 sequence, f32 frame);
    /** Selects one node's object run without disturbing independently posed siblings. */
    void setMeshPresentationFrame(usize index, u32 sequence, f32 frame);
    /** Whether a frame's mesh may be blended towards the next's at all: as many parts, lit
     * alike. Vertices, texture coordinates, colours and textures may all differ; morphMap
     * decides whether the vertices can be matched. */
    static bool compatibleMorph(const Mesh& from, const Mesh& to);
    /**
     * The vertex of `to` each vertex of `from` blends towards, for frames exported with
     * their vertices in another order: the nearest with the same texture coordinate and
     * colour. Empty when the orders agree; none when some vertex has no such partner or
     * the nearest stands too far off to be the same one.
     */
    static std::optional<std::vector<u32>> morphMap(const Mesh& from, const Mesh& to);
    /** Overrides object-animation frames for just one independently animated branch. */
    void setSubtreeFrame(usize root, u32 sequence, s32 frame);

    /** Draws with `model` placing model space in the world and `clip` mapping the world to
     * clip space: the nodes drawn at once first, then those the authored flags defer (a
     * sortable node that is faded, glowing, flagged behind or alpha-textured, the way the
     * original's MBSetupObject decides), every part with the console's alpha compare.
     * `nodeTransforms`, one matrix per tree node in model space, poses the figure; empty, it
     * stands at rest. An `alpha` under one blends every part that much (writing no depth);
     * at zero nothing is drawn. */
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
              const WorldLighting& lighting = {}, std::span<const Mat4> nodeTransforms = {},
              const CameraFrame* camera = nullptr, f32 alpha = 1.0f, Pass pass = Pass::All,
              Occlusion occlusion = Occlusion::Authored) const;

private:
    /** A mesh and how its parts draw. */
    struct Shape {
        const Mesh* mesh = nullptr;
        std::vector<const Texture*> textures; ///< one per mesh part
        std::vector<bool> translucent;        ///< one per mesh part
        std::vector<u32> slots;               ///< the set's texture index, one per part
        /** An authored silhouette (an indexed mask whose palette shows nothing, see
         * TextureSet::silhouette) cuts the flat part it lies over instead of drawing: the
         * cut per part, the parts that are masks, and every vertex's place in the mask. */
        std::vector<const Texture*> cutouts;
        std::vector<bool> hidden;
        std::vector<Vec2> cutoutUv;
    };
    /** An object node's meshes for one sequence: the one shown at `start` and each frame
     * after, in order; none for a sequence it shows nothing in. */
    struct FrameRun {
        s32 start = 0;
        s32 reverseLength = 0; ///< sequence extent when object frames play backward
        std::vector<Shape> shapes;
        std::vector<bool> morphs; ///< compatible geometry/materials between adjacent frames
        /** Per adjacent pair, the vertex map ahead and back (empty: the same order). */
        std::vector<std::vector<u32>> forward;
        std::vector<std::vector<u32>> backward;
    };
    struct TextureFrame {
        u32 slot = 0;
        const Texture* frame = nullptr;
        const Texture* next = nullptr;
        f32 blend = 0.0f;
    };
    struct Node {
        Shape shape; ///< what the node draws now; without a mesh, nothing
        const Mesh* nextMesh = nullptr;
        const std::vector<u32>* nextMap = nullptr; ///< into nextMesh, when its order differs
        f32 meshBlend = 0.0f;
        usize index = 0; ///< the tree node this mesh hangs from
        Vec3 offset{0.0f, 0.0f, 0.0f};
        bool chrome = false;
        bool additive = false; ///< added onto the frame, after the opaque
        bool depthWrite = true;
        bool depthTest = true;
        bool sorted = false;          ///< eligible for deferral (flag 0x800)
        bool deferAlways = false;     ///< a glow or sort-behind node: deferred while sorted
        bool behind = false;          ///< a lowered sort key: after the ordinary deferred
        u32 facing = 0;               ///< turned to the camera this way, when given one
        std::vector<FrameRun> runs;   ///< an object node's, one per sequence
        std::vector<usize> ancestors; ///< includes this node, then its parents
        std::vector<TextureFrame> frames;
        std::optional<Vec2> uvOffset;
        Vec2 uvScale{1.0f};
        f32 alpha = 1.0f;
        const Texture* maskedTexture = nullptr;
    };

    static Shape makeShape(const Mesh& mesh, TextureSet& textures, RenderDevice& device,
                           std::span<TextureSet* const> lenders);
    /** Turns the shape's silhouette masks into cutouts of the parts they lie over. */
    static void cutBySilhouettes(Shape& shape, TextureSet& textures, RenderDevice& device,
                                 std::span<TextureSet* const> lenders);
    static void selectFrame(Node& node, u32 sequence, s32 frame);
    static void selectPresentationFrame(Node& node, u32 sequence, f32 frame);
    /** Grows the bounds around `shape` at `offset`. */
    void include(const Shape& shape, const Vec3& offset, bool& first);

    void drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                   const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                   const CameraFrame* camera, f32 alpha, bool translucent, Pass pass,
                   Occlusion occlusion) const;

    std::vector<Node> m_nodes;
    const Texture* m_maskedTexture = nullptr;
    SkinAlpha m_skinAlpha = SkinAlpha::PreserveCoverage;
    bool m_unlit = false;
    bool m_additive = false;
    bool m_depthWrite = true;
    bool m_fadedDepthWrite = false;
    bool m_cullBack = true;
    Color m_tint = Color::white();
    f32 m_whiteBlend = 0.0f;
    const SelectionHighlight* m_highlight = nullptr;
    std::vector<TextureFrame> m_frames;
    struct TextureSampling {
        u32 slot;
        bool clamp;
        bool mipmaps;
    };
    std::vector<TextureSampling> m_textureSampling;
    /** A slot's coordinates slid and stretched. */
    struct Slide {
        u32 slot = 0;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
    };
    std::vector<Slide> m_offsets;
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
