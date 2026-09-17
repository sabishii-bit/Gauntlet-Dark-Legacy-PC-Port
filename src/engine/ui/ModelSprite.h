#pragma once

#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

namespace gdl {

/**
 * Draws a static animation tree (its meshes at their node offsets) as a lit 3D object at a
 * position on the virtual screen, in front of the canvas' sprites. Chrome nodes take their
 * texture coordinates from the view-space normal, like the original's environment mapping.
 */
class ModelSprite {
public:
    static constexpr f32 kDepth = 0.75f;      ///< canvas depth the model is centred on
    static constexpr f32 kDepthRange = 0.02f; ///< depth per model unit, nearer is larger

    /** Gathers the tree's meshes and textures; false (with a warning) when any is missing. */
    bool bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures, RenderDevice& device);

    bool bound() const { return !m_nodes.empty(); }
    usize nodeCount() const { return m_nodes.size(); }

    /**
     * Draws at `position` (virtual pixels) with `scale` pixels per model unit, turned by `pitch`
     * radians about the screen's horizontal axis; `orientation` maps model space to the screen
     * (x right, y up, z towards the viewer) before that.
     */
    void draw(Canvas& canvas, Vec2 position, f32 scale, f32 pitch,
              const Mat4& orientation = Mat4{1.0f}) const;

private:
    struct Node {
        const Mesh* mesh = nullptr;
        Vec3 offset{0.0f, 0.0f, 0.0f};
        bool chrome = false;
        std::vector<const Texture*> textures; ///< one per mesh part
    };

    std::vector<Node> m_nodes;
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
