#include "game/level/StaticScenery.h"

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool StaticScenery::digestLocalValues(NativeStateDigest& digest, const LocalValues& state) const {
    digest.string("StaticScenery.native.1");
    // These immutable binding inputs remain fixed until clear/bind invalidates
    // history. Mutable collision values are visited by the enclosing world owner.
    digest.resource(m_layout);
    digest.resource(m_collision);
    digest.fields<1>(state);
    return digest.good();
}

bool StaticScenery::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& prop : m_sim.props) {
        if (!prop.figure.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void StaticScenery::bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
                         const WorldCollision* collision) {
    clear();
    m_layout = &layout;
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
        if (info.type != ItemInfo::kObstacle ||
            (positiveOverride ? static_cast<s32>(override) : info.subtype) != 0) {
            continue;
        }
        Prop prop;
        prop.instance = static_cast<s32>(index);
        prop.minPlayers = instance.minPlayers;
        ItemInstance placed = instance;
        // AddItemSub uses the initialized world, within four units above/ten below.
        if (collision != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
            if (const auto floor = collision->floorAt(instance.position, 4, 10)) {
                placed.position.y = floor->y + ItemFigure::kFloorLift;
            }
        }
        if ((instance.flags & ItemInstance::kNoGeometry) == 0) {
            prop.figure.placeStaticFallback(device, scope, info.name, placed, nullptr,
                                            info.objectFlags);
        }
        // NoGeometry suppresses the mesh, not the item's authored collision/placement.
        prop.figure.placeAt(itemPlacement(placed.position, placed.rotation));
        prop.box = prop.figure.obstacle(info);
        prop.box.centre = Vec3{prop.figure.transform() * Vec4{info.collisionOffset, 1}};
        prop.box.solid = info.collisionType != 0;
        if (info.collisionType == 1) {
            prop.box.cylinderRadius = info.radius;
        }
        prop.support.bind(instance, info, collision, prop.figure, prop.box);
        m_sim.props.push_back(std::move(prop));
    }
    setPlayerCount(1);
}

void StaticScenery::clear() {
    m_localBinding.rebind();
    m_layout = nullptr;
    m_sim.props.clear();
    m_collision = nullptr;
}

void StaticScenery::setPlayerCount(s32 players) {
    for (auto& prop : m_sim.props) {
        prop.shown = shownToParty(prop.minPlayers, players);
    }
}

void StaticScenery::capturePresentation() {
    for (auto& prop : m_sim.props) {
        prop.figure.capturePresentation();
    }
}

void StaticScenery::syncFloors() {
    for (auto& prop : m_sim.props) {
        prop.support.sync(m_collision, prop.figure, prop.box);
    }
}

void StaticScenery::update(f32 seconds) {
    syncFloors();
    for (auto& prop : m_sim.props) {
        prop.figure.update(seconds);
    }
}

std::vector<Obstacle> StaticScenery::obstacles() const {
    std::vector<Obstacle> result;
    for (const auto& prop : m_sim.props) {
        if (prop.shown && prop.box.solid) {
            result.push_back(prop.box);
        }
    }
    return result;
}

void StaticScenery::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera, TreeModel::Pass pass,
                         f32 presentationAlpha) const {
    for (const auto& prop : m_sim.props) {
        if (prop.shown) {
            prop.figure.draw(device, clip, lighting, 1, 1, camera, pass, presentationAlpha);
        }
    }
}
} // namespace gdl::game
