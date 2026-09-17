#pragma once

#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"

namespace gdl {

/** Simple lighting for a level: a constant term plus one directional light. */
struct WorldLighting {
    f32 ambient = 0.55f;
    f32 diffuse = 0.45f;
    Vec3 direction{0.3f, 1.0f, 0.2f}; ///< towards the light
};

/**
 * A level's static geometry, placed by its layout and gathered per texture so the whole
 * world draws in a few batches. Lighting is applied per vertex when the scene is built.
 */
class WorldScene {
public:
    /** Gathers every placed object that has a mesh; false when nothing could be placed. */
    bool build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
               RenderDevice& device, const WorldLighting& lighting = {});

    void clear();
    bool built() const { return !m_batches.empty(); }
    usize placedCount() const { return m_placed; }
    usize batchCount() const { return m_batches.size(); }
    usize triangleCount() const { return m_triangles; }

    /** Draws opaque geometry, then translucent, with `clip` mapping world to clip space. */
    void draw(RenderDevice& device, const Mat4& clip) const;

private:
    struct Batch {
        const Texture* texture = nullptr;
        ImmediateBatch geometry;
        bool translucent = false;
    };

    std::vector<Batch> m_batches;
    usize m_placed = 0;
    usize m_triangles = 0;
};

} // namespace gdl
