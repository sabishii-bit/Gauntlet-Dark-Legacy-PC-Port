#include "game/world/MusicAreas.h"

#include <cstring>

#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
constexpr usize kAreaOffset = 4;   ///< the area, a 32-bit word after the radius
constexpr usize kSwitchOffset = 8; ///< the way over, a 16-bit word after the area
} // namespace

s32 MusicAreas::areaOf(const ItemInstance& instance) {
    s32 area = 0;
    std::memcpy(&area, &instance.params[kAreaOffset], sizeof(area));
    // The original keeps the word as a 16-bit field.
    return static_cast<s16>(area);
}

MusicSwitch MusicAreas::switchOf(const ItemInstance& instance) {
    s16 how = 0;
    std::memcpy(&how, &instance.params[kSwitchOffset], sizeof(how));
    if (how <= 0) {
        return MusicSwitch::AtPartEnd;
    }
    return how == 1 ? MusicSwitch::Faded : MusicSwitch::AtOnce;
}

bool MusicAreas::bind(const WorldLayout& layout, const WorldScene* world) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != kSoundItem) {
            continue;
        }
        const s32 area = areaOf(instance);
        if (area <= 0) {
            continue;
        }
        MusicZone zone;
        zone.instance = static_cast<s32>(i);
        zone.minPlayers = instance.minPlayers;
        zone.position = instance.position;
        constexpr f32 kParentReach = 10;
        f32 nearest = kParentReach;
        for (const auto& animation : layout.animations()) {
            if (animation.object < 0 ||
                static_cast<usize>(animation.object) >= layout.objects().size()) {
                continue;
            }
            const auto object = static_cast<usize>(animation.object);
            const Vec3 position = world != nullptr ? Vec3{world->worldTransform(object)[3]}
                                                   : layout.worldPosition(object);
            const f32 distance = glm::distance(instance.position, position);
            if (distance < nearest) {
                nearest = distance;
                zone.parent = animation.object;
            }
        }
        std::memcpy(&zone.radius, instance.params.data(), sizeof(zone.radius));
        zone.area = area - 1;
        zone.how = switchOf(instance);
        m_zones.push_back(zone);
    }
    return !m_zones.empty();
}

void MusicAreas::clear() {
    m_zones.clear();
    m_players = 1;
}

std::optional<MusicCue> MusicAreas::pick(std::span<const Vec3> listeners,
                                         const WorldScene* world) const {
    std::optional<MusicCue> best;
    for (const MusicZone& zone : m_zones) {
        if (!shownToParty(zone.minPlayers, m_players) ||
            (best.has_value() && zone.area <= best->area)) {
            continue;
        }
        // A sound item uses its parent's center, rather than retaining a local offset.
        const Vec3 center = world != nullptr && zone.parent >= 0
                                ? Vec3{world->worldTransform(static_cast<usize>(zone.parent))[3]}
                                : zone.position;
        for (const Vec3& listener : listeners) {
            if (glm::distance(listener, center) < zone.radius) {
                best = MusicCue{zone.area, zone.how};
                break;
            }
        }
    }
    return best;
}

std::optional<MusicCue> MusicAreas::update(std::span<const Vec3> listeners, s32 current,
                                           const WorldScene* world) const {
    const std::optional<MusicCue> cue = pick(listeners, world);
    if (!cue.has_value() || cue->area == current) {
        return std::nullopt;
    }
    return cue;
}

} // namespace gdl::game
