#pragma once

#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game {

/** WorldObjectExplode / DoWorldAnimSub: carts and explosive scenery disappear along
 * their parent chain. A looping cart reappears at the start of its next pass. */
class WorldDestruction {
public:
    void bind(const WorldLayout& layout);
    void clear();
    std::span<const usize> controlledObjects() const { return m_controlled; }
    bool destroyed(s32 object) const;
    bool explode(s32 object, const Vec3& position, WorldScene& scene, WorldCollision& collision);
    void update(std::span<const WorldAnimator::CycleEvent> events, WorldScene& scene,
                WorldCollision& collision);
    std::vector<Vec3> takeExplosions();

private:
    bool valid(s32 object) const;
    bool below(usize descendant, s32 ancestor) const;
    void showSubtree(s32 object, bool visible, WorldScene& scene);
    std::vector<WorldObject> m_objects;
    std::vector<bool> m_destroyed;
    std::vector<usize> m_controlled;
    std::vector<Vec3> m_explosions;
};
} // namespace gdl::game
