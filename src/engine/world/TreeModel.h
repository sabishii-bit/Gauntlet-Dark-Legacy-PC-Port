#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"
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
    /** Depthless effects composite after scenery; solid parts still occlude actors. */
    enum class Pass : u8 { All, DepthWriting, Effects };
    /** World-bound shots must test the already-rendered solid scene, including
     * halo nodes whose authored flags otherwise disable the depth test. */
    enum class Occlusion : u8 { Authored, SolidWorld };
    /** Gathers meshes and textures, resolving external slots through named lenders.
     * The mesh and texture owners must outlive this model. Missing resources fail the bind. */
    bool bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures, RenderDevice& device,
              std::span<TextureSet* const> lenders = {});

    void clear() {
        m_nodes.clear();
        resetTextures();
        setAppearance(false);
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
    /** Applies an alternate appearance without making solid skin translucent or filling
     * its cutouts. Cleared by resetTextures(). */
    void setMaskedTexture(const Texture* texture) { m_maskedTexture = texture; }
    /** Full ambient illumination and an optional RGB tint applied to every node. */
    void setAppearance(bool unlit, Color tint = Color::white(), bool depthWrite = true,
                       bool additive = false) {
        m_unlit = unlit;
        m_tint = tint;
        m_depthWrite = depthWrite;
        m_additive = additive;
    }
    /** Draws both faces of every part: a flat decal seen from either side. */
    void setDoubleSided(bool doubleSided) { m_cullBack = !doubleSided; }
    Vec2 textureOffset(u32 slot) const;
    /** How a slot's coordinates are stretched, one and one when they are not. */
    Vec2 textureScale(u32 slot) const;

    /** Shows the object nodes' meshes for `frame` of `sequence`: the run's mesh for the
     * frame, the only mesh of a one-frame run, else none. */
    void setFrame(u32 sequence, s32 frame);
    /** Opt-in visual mesh morphing for authored vertex-animation runs. Incompatible
     * adjacent topology/material/UV frames remain discrete. */
    void setPresentationFrame(u32 sequence, f32 frame);
    static bool compatibleMorph(const Mesh& from, const Mesh& to);
    /** Overrides object-animation frames for just one independently animated branch. */
    void setSubtreeFrame(usize root, u32 sequence, s32 frame);

    /** Draws with `model` placing model space in the world and `clip` mapping the world to
     * clip space; opaque parts first, then translucent ones. `nodeTransforms`, one matrix per
     * tree node in model space, poses the figure; empty, it stands at rest. An `alpha` under
     * one blends every part that much (writing no depth); at zero nothing is drawn. */
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
    };
    /** An object node's meshes for one sequence: the one shown at `start` and each frame
     * after, in order; none for a sequence it shows nothing in. */
    struct FrameRun {
        s32 start = 0;
        s32 reverseLength = 0; ///< sequence extent when object frames play backward
        std::vector<Shape> shapes;
        std::vector<bool> morphs; ///< compatible geometry/materials between adjacent frames
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
        f32 meshBlend = 0.0f;
        usize index = 0; ///< the tree node this mesh hangs from
        Vec3 offset{0.0f, 0.0f, 0.0f};
        bool chrome = false;
        bool additive = false; ///< added onto the frame, after the opaque
        bool depthWrite = true;
        bool depthTest = true;
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
    static void selectFrame(Node& node, u32 sequence, s32 frame);
    /** Grows the bounds around `shape` at `offset`. */
    void include(const Shape& shape, const Vec3& offset, bool& first);

    void drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                   const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                   const CameraFrame* camera, f32 alpha, bool translucent, Pass pass,
                   Occlusion occlusion) const;

    std::vector<Node> m_nodes;
    const Texture* m_maskedTexture = nullptr;
    bool m_unlit = false;
    bool m_additive = false;
    bool m_depthWrite = true;
    bool m_cullBack = true;
    Color m_tint = Color::white();
    std::vector<TextureFrame> m_frames;
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
