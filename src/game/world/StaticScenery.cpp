#include "game/world/StaticScenery.h"

#include "engine/core/Types.h"

namespace gdl::game {
void StaticScenery::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                         const WorldCollision* collision, ItemArchive* realmItems) {
    clear();
    m_collision = collision;
    for (usize index = 0; index < layout.itemInstances().size(); ++index) {
        const auto& instance = layout.itemInstances()[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= layout.itemInfos().size()) {
            continue;
        }
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        const u32 override =
            static_cast<u32>(instance.params[0]) | (static_cast<u32>(instance.params[1]) << 8U);
        const bool positiveOverride = override > 0 && override < 0x8000U;
        if (info.type != 10 ||
            (positiveOverride ? static_cast<s32>(override) : info.subtype) != 0) {
            continue;
        }
        auto prop = std::make_unique<Prop>();
        prop->instance = static_cast<s32>(index);
        prop->minPlayers = instance.minPlayers;
        ItemInstance placed = instance;
        // AddItemSub uses the initialized world, within four units above/ten below.
        if (collision != nullptr && (info.collisionFlags & 1U) == 0) {
            if (const auto floor = collision->floorAt(instance.position, 4, 10)) {
                placed.position.y = floor->y + ItemFigure::kFloorLift;
            }
        }
        if ((instance.flags & 2U) == 0) {
            if (!prop->figure.placeStaticFallback(device, items, info.name, placed, nullptr,
                                                  info.objectFlags) &&
                realmItems != nullptr) {
                prop->figure.placeStaticFallback(device, *realmItems, info.name, placed, nullptr,
                                                 info.objectFlags);
            }
        }
        // NoGeometry suppresses the mesh, not the item's authored collision/placement.
        prop->figure.placeAt(itemPlacement(placed.position, placed.rotation));
        prop->box = prop->figure.obstacle(info);
        prop->box.centre = Vec3{prop->figure.transform() * Vec4{info.collisionOffset, 1}};
        prop->box.solid = info.collisionType != 0;
        if (info.collisionType == 1) {
            prop->box.cylinderRadius = info.radius;
        }
        prop->support.bind(instance, info, collision, prop->figure, prop->box);
        m_props.push_back(std::move(prop));
    }
    setPlayerCount(1);
}

void StaticScenery::clear() {
    m_props.clear();
    m_collision = nullptr;
}

void StaticScenery::setPlayerCount(s32 players) {
    for (auto& prop : m_props) {
        prop->shown = shownToParty(prop->minPlayers, players);
    }
}

void StaticScenery::capturePresentation() {
    for (auto& prop : m_props) {
        prop->figure.capturePresentation();
    }
}

void StaticScenery::syncFloors() {
    for (auto& prop : m_props) {
        prop->support.sync(m_collision, prop->figure, prop->box);
    }
}

void StaticScenery::update(f32 seconds) {
    syncFloors();
    for (auto& prop : m_props) {
        prop->figure.update(seconds);
    }
}

std::vector<Obstacle> StaticScenery::obstacles() const {
    std::vector<Obstacle> result;
    for (const auto& prop : m_props) {
        if (prop->shown && prop->box.solid) {
            result.push_back(prop->box);
        }
    }
    return result;
}

void StaticScenery::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera, TreeModel::Pass pass,
                         f32 presentationAlpha) const {
    for (const auto& prop : m_props) {
        if (prop->shown) {
            prop->figure.draw(device, clip, lighting, 1, 1, camera, pass, presentationAlpha);
        }
    }
}
} // namespace gdl::game
