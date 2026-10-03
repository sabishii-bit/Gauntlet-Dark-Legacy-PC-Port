#include "game/world/ItemSupport.h"

#include <optional>

#include "engine/core/Types.h"

namespace gdl::game {
std::optional<WorldCollision> itemSupportWorld(const WorldLayout& layout,
                                               const WorldCollision* collision) {
    if (collision == nullptr || collision->movingObjectCount() == 0) {
        return std::nullopt;
    }
    auto result = *collision;
    for (usize i = 0; i < layout.objects().size(); ++i) {
        result.setObjectTransform(static_cast<s32>(i),
                                  glm::translate(Mat4{1}, layout.worldPosition(i)));
    }
    return result;
}

void ItemSupport::bind(const ItemInstance& instance, const ItemInfo& info,
                       const WorldCollision* authored, const ItemFigure& figure,
                       const Obstacle& box) {
    m_object = -1;
    if (authored == nullptr || (info.collisionFlags & 1U) != 0) {
        return;
    }
    const auto floor = authored->floorAt(instance.position, 4, 10);
    if (!floor) {
        return;
    }
    const auto transform = authored->objectTransform(floor->object);
    if (!transform) {
        return;
    }
    Vec3 position = instance.position;
    position.y = floor->y + ItemFigure::kFloorLift;
    m_object = floor->object;
    m_local = glm::inverse(*transform) * itemPlacement(position, instance.rotation);
    m_boxOffset = Vec3{glm::inverse(figure.transform()) * Vec4{box.centre, 1}};
    m_boxYaw = box.yaw - figure.yaw();
}

void ItemSupport::sync(const WorldCollision* collision, ItemFigure& figure, Obstacle& box) {
    if (collision == nullptr || m_object < 0) {
        return;
    }
    const auto transform = collision->objectTransform(m_object);
    // A trigger can disable collision while its floor animates. MBNodeSetParent
    // still carries its children; collision enablement is not attachment lifetime.
    if (!transform) {
        m_object = -1;
        return;
    }
    figure.placeAt(*transform * m_local);
    box.centre = Vec3{figure.transform() * Vec4{m_boxOffset, 1}};
    box.yaw = figure.yaw() + m_boxYaw;
}
} // namespace gdl::game
