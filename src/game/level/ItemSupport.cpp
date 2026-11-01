#include "game/level/ItemSupport.h"

#include <cmath>
#include <optional>

#include "engine/core/Types.h"

namespace gdl::game {
/** The authored pose's floor must lie this much nearer an item's height than the
 * initialized world's to be taken for the floor the item was set on. */
constexpr f32 kRestPosePreference = 1.0f;

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

const WorldCollision* itemSupportAt(const Vec3& position, const WorldCollision* current,
                                    const WorldCollision* authored) {
    // FloorPos rests an item on the floor at its authored height: a chest set on a lift's
    // lowered deck finds it in the initialized world, a runestone set on a platform at rest
    // (the Roots' lift starts raised thirty over it) finds it in the authored one. Of the
    // two, the floor nearer the authored height is the one the item was set on.
    const auto nearest = [&](const WorldCollision* world) -> std::optional<f32> {
        if (world == nullptr) {
            return std::nullopt;
        }
        const auto floor = world->restingFloorAt(position, 4, 10, 1);
        return floor ? std::optional{std::abs(floor->y - position.y)} : std::nullopt;
    };
    const auto here = nearest(current);
    const auto rest = nearest(authored);
    if (here && (!rest || *here <= *rest + kRestPosePreference)) {
        return current;
    }
    return authored;
}

void ItemSupport::bind(const ItemInstance& instance, const ItemInfo& info,
                       const WorldCollision* authored, const ItemFigure& figure,
                       const Obstacle& box, const WorldCollision* current) {
    m_object = -1;
    authored = itemSupportAt(instance.position, current, authored);
    if (authored == nullptr || (info.collisionFlags & ItemInfo::kKeepsHeight) != 0) {
        return;
    }
    const auto floor = authored->restingFloorAt(instance.position, 4, 10, 1);
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
