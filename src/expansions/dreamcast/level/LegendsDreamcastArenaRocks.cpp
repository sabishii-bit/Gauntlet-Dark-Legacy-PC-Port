#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"

#include <bit>
#include <cmath>
#include <format>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {

std::optional<LegendsDreamcastArenaRock>
legendsDreamcastArenaRock(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, usize standing,
                          std::string* diagnostic) {
    if (diagnostic) {
        diagnostic->clear();
    }
    // A positive subtype in the placement's first parameter replaces the definition's.
    const s16 placed = std::bit_cast<s16>(readU16LE(instance.params, 0));
    if (info.type != 10 || (placed > 0 ? placed : info.subtype) != 41) {
        return std::nullopt;
    }
    const auto reject = [&](std::string reason) -> std::optional<LegendsDreamcastArenaRock> {
        if (diagnostic) {
            *diagnostic = std::move(reason);
        }
        return std::nullopt;
    };
    // SAFEROCK<n>0 is the rubble; each standing segment has its own model up to 3.
    constexpr s16 kMostSegments = 3;
    // Far above any shipped rock's 40; larger values are not plausible native data.
    constexpr s16 kMostSegmentHealth = 10000;
    const s16 tier = std::bit_cast<s16>(readU16LE(instance.params, 2));
    if (tier < 0 || tier > kMostSegments) {
        return reject("Dreamcast arena rock has an unsupported segment count");
    }
    if (info.hitPoints <= 0 || info.hitPoints > kMostSegmentHealth) {
        return reject("Dreamcast arena rock has unsupported durability");
    }
    if (info.collisionType != 1 || !std::isfinite(info.radius) || info.radius <= 0 ||
        !std::isfinite(info.height) || info.height <= 0 || info.collisionOffset != Vec3{0}) {
        return reject("Dreamcast arena rock has an unsupported collision body");
    }
    if (standing >= kMostDreamcastArenaRocks) {
        return reject(std::format("Dreamcast arena has more safe rocks than the boss's {}",
                                  kMostDreamcastArenaRocks));
    }
    LegendsDreamcastArenaRock rock;
    rock.body = {instance.position, instance.rotation.y, info.radius, info.height, info.hitPoints};
    rock.tier = tier;
    rock.armor = info.armor;
    rock.dormant = (instance.flags & 2U) != 0;
    return rock;
}

} // namespace gdl::game
