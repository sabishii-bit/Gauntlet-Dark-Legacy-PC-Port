#pragma once

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
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

namespace gdl {

/**
 * An animation tree standing in the world: each node's mesh at the node's rest offset or
 * wherever a pose's matrices put it, lit like the level and drawn wherever a model matrix
 * places the whole figure.
 */
class TreeModel {
public:
    /** Gathers the tree's meshes and textures; false (with a warning) when any is missing. */
    bool bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures, RenderDevice& device);

    void clear() { m_nodes.clear(); }
    bool bound() const { return !m_nodes.empty(); }
    usize nodeCount() const { return m_nodes.size(); }

    /** The rest pose's extent in model space. */
    const Vec3& minBounds() const { return m_min; }
    const Vec3& maxBounds() const { return m_max; }

    /** Draws with `model` placing model space in the world and `clip` mapping the world to
     * clip space; opaque parts first, then translucent ones. `nodeTransforms`, one matrix per
     * tree node in model space, poses the figure; empty, it stands at rest. An `alpha` under
     * one blends every part that much (writing no depth); at zero nothing is drawn. */
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
              const WorldLighting& lighting = {}, std::span<const Mat4> nodeTransforms = {},
              const CameraFrame* camera = nullptr, f32 alpha = 1.0f) const;

private:
    struct Node {
        const Mesh* mesh = nullptr;
        usize index = 0; ///< the tree node this mesh hangs from
        Vec3 offset{0.0f, 0.0f, 0.0f};
        bool chrome = false;
        bool additive = false;   ///< added onto the frame, after the opaque
        bool depthWrite = true;
        u32 facing = 0;          ///< turned to the camera this way, when given one
        std::vector<const Texture*> textures; ///< one per mesh part
        std::vector<bool> translucent;        ///< one per mesh part
    };

    void drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                   const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                   const CameraFrame* camera, f32 alpha, bool translucent) const;

    std::vector<Node> m_nodes;
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
