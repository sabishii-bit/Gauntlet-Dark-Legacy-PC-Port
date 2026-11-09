#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/world/WorldCollision.h"

#include "formats/WorldFile.h"
#include "game/level/ImportedTargets.h"

namespace gdl::game {

/** Dreamcast shoot walls are items with local triangle meshes, not world
 * objects. Keep their bodies, attack surfaces and lifetime together. */
class LegendsDreamcastScenery {
public:
    void add(const formats::ItemInfoRecord& info, const formats::ItemInstanceRecord& item,
             usize figure, s32 object, std::span<const formats::WorldCollisionTriangle> faces);
    std::vector<ImportedTarget> targets() const;
    bool strike(s32 target, f32 damage, u32 flags);
    std::optional<s32> targetForObject(s32 object) const;
    void update(f32 seconds);
    bool visible(usize figure) const;
    f32 whiteBlend(usize figure) const;
    s32 health(s32 target) const;
    Vec3 position(s32 target) const;
    void bindCollision(WorldCollision& collision) const;
    void syncCollision(WorldCollision& collision) const;

private:
    struct Wall {
        usize figure = 0;
        s32 object = -1;
        s32 health = 0;
        s32 armor = 0;
        f32 flash = 0;
        Vec3 minimum{0}, maximum{0};
        std::vector<CollisionTriangle> surface;
    };
    std::vector<Wall> m_walls;
};

} // namespace gdl::game
