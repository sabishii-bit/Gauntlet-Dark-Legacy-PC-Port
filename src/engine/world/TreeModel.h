#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
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
    /** Gathers the tree's meshes and textures; false (with a warning) when any is missing. */
    bool bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures, RenderDevice& device);

    void clear() {
        m_nodes.clear();
        resetTextures();
        setAppearance(false);
    }
    bool bound() const { return !m_nodes.empty(); }
    std::size_t nodeCount() const { return m_nodes.size(); }

    /** The rest pose's extent in model space. */
    const Vec3& minBounds() const { return m_min; }
    const Vec3& maxBounds() const { return m_max; }

    /** Shows `frame` wherever the parts use texture `slot` of the set (null: the set's own),
     * or slides their coordinates by `offset`, the way texture animations move. */
    void setTextureFrame(std::uint32_t slot, const Texture* frame);
    void setTextureOffset(std::uint32_t slot, const Vec2& offset,
                          const Vec2& scale = Vec2{1.0f, 1.0f});
    void resetTextures();
    /** Applies an alternate appearance without making solid skin translucent or filling
     * its cutouts. Cleared by resetTextures(). */
    void setMaskedTexture(const Texture* texture) { m_maskedTexture = texture; }
    /** Full ambient illumination and an optional RGB tint applied to every node. */
    void setAppearance(bool unlit, Color tint = Color::white(), bool depthWrite = true) {
        m_unlit = unlit;
        m_tint = tint;
        m_depthWrite = depthWrite;
    }
    Vec2 textureOffset(std::uint32_t slot) const;
    /** How a slot's coordinates are stretched, one and one when they are not. */
    Vec2 textureScale(std::uint32_t slot) const;

    /** Shows the object nodes' meshes for `frame` of `sequence`: the run's mesh for the
     * frame, the only mesh of a one-frame run, else none. */
    void setFrame(std::uint32_t sequence, std::int32_t frame);

    /** Draws with `model` placing model space in the world and `clip` mapping the world to
     * clip space; opaque parts first, then translucent ones. `nodeTransforms`, one matrix per
     * tree node in model space, poses the figure; empty, it stands at rest. An `alpha` under
     * one blends every part that much (writing no depth); at zero nothing is drawn. */
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
              const WorldLighting& lighting = {}, std::span<const Mat4> nodeTransforms = {},
              const CameraFrame* camera = nullptr, float alpha = 1.0f) const;

private:
    /** A mesh and how its parts draw. */
    struct Shape {
        const Mesh* mesh = nullptr;
        std::vector<const Texture*> textures; ///< one per mesh part
        std::vector<bool> translucent;        ///< one per mesh part
        std::vector<std::uint32_t> slots;     ///< the set's texture index, one per part
    };
    /** An object node's meshes for one sequence: the one shown at `start` and each frame
     * after, in order; none for a sequence it shows nothing in. */
    struct FrameRun {
        std::int32_t start = 0;
        std::vector<Shape> shapes;
    };
    struct Node {
        Shape shape;           ///< what the node draws now; without a mesh, nothing
        std::size_t index = 0; ///< the tree node this mesh hangs from
        Vec3 offset{0.0f, 0.0f, 0.0f};
        bool chrome = false;
        bool additive = false; ///< added onto the frame, after the opaque
        bool depthWrite = true;
        std::uint32_t facing = 0;   ///< turned to the camera this way, when given one
        std::vector<FrameRun> runs; ///< an object node's, one per sequence
    };

    static Shape makeShape(const Mesh& mesh, TextureSet& textures, RenderDevice& device);
    /** Grows the bounds around `shape` at `offset`. */
    void include(const Shape& shape, const Vec3& offset, bool& first);

    void drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                   const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                   const CameraFrame* camera, float alpha, bool translucent) const;

    std::vector<Node> m_nodes;
    const Texture* m_maskedTexture = nullptr;
    bool m_unlit = false;
    bool m_depthWrite = true;
    Color m_tint = Color::white();
    std::vector<std::pair<std::uint32_t, const Texture*>> m_frames; ///< slot, frame shown
    /** A slot's coordinates slid and stretched. */
    struct Slide {
        std::uint32_t slot = 0;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
    };
    std::vector<Slide> m_offsets;
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
