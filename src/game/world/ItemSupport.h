#pragma once

#include <optional>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
/** SetItem/AddItemSub parents floor-placed items to animated world nodes before
 * world animations move them. Query that authored world, not the initial animation pose. */
std::optional<WorldCollision> itemSupportWorld(const WorldLayout& layout,
                                               const WorldCollision* collision);

/** The same floor-local placement drives an item's figure and collision during gameplay
 * and camera-only updates. Collision flag 1 deliberately forbids floor placement. */
class ItemSupport {
public:
    void bind(const ItemInstance& instance, const ItemInfo& info, const WorldCollision* authored,
              const ItemFigure& figure, const Obstacle& box);
    void sync(const WorldCollision* collision, ItemFigure& figure, Obstacle& box);
    s32 object() const { return m_object; }

private:
    s32 m_object = -1;
    Mat4 m_local{1};
    Vec3 m_boxOffset{0};
    f32 m_boxYaw = 0;
};
} // namespace gdl::game
