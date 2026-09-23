#pragma once

#include <random>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {
/** The altar's loose masonry, jolted twice then scattered by Skorne's entrance.
 * Borrows level meshes/textures; clear before their archives are released. */
class SkorneArena {
public:
    void bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
              TextureSet& textures);
    void clear();
    void cue(const Vec3& boss);
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    usize size() const { return m_rocks.size(); }
    s32 phase() const { return m_phase; }
    Vec3 position(usize index) const { return m_rocks.at(index).position; }
    bool visible(usize index) const { return m_rocks.at(index).visible; }

private:
    struct Rock {
        TreeModel model;
        Vec3 position{0};
        Vec3 rotation{0};
        Vec3 velocity{0};
        usize instance = 0;
        bool visible = true;
    };
    std::vector<Rock> m_rocks;
    std::mt19937 m_random{0x534b4f52U};
    s32 m_phase = 0;
    f32 m_remainder = 0;
    f32 m_bottom = 0;
};
} // namespace gdl::game
