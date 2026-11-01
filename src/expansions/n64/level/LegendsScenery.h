#pragma once

#include <span>
#include <vector>

#include "engine/world/WorldCollision.h"

#include "expansions/shared/formats/LegendsMap.h"
#include "game/level/ImportedTargets.h"

namespace gdl::game {

/** Mutable native SWAL health, separate from static geometry and the GDL campaign. */
class LegendsScenery {
public:
    static constexpr s32 kTargetBase = kImportedSceneryBase;
    LegendsScenery() = default;
    LegendsScenery(std::span<const formats::LegendsMapDestructible> definitions, f32 stageScale,
                   std::span<const formats::LegendsMapCollisionTriangle> faces = {});
    std::vector<ImportedTarget> targets() const;
    /** Physical GDL attacks feed native integer health; magic-only attacks do not. */
    bool strike(s32 target, f32 damage, u32 flags);
    void update(f32 seconds);
    f32 whiteBlend(usize object) const;
    bool visible(usize object) const;
    void syncCollision(WorldCollision& collision) const;
    s32 health(usize slot) const;
    std::span<const formats::LegendsMapDestructible> definitions() const { return m_definitions; }

private:
    std::vector<formats::LegendsMapDestructible> m_definitions;
    std::vector<s32> m_health;
    std::vector<f32> m_flash;
    std::vector<std::vector<CollisionTriangle>> m_surfaces;
};

} // namespace gdl::game
