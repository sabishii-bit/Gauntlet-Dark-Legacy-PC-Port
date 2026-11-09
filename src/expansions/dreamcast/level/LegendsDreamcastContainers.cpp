#include "expansions/dreamcast/level/LegendsDreamcastContainers.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "engine/core/Error.h"

namespace gdl::game {
namespace {
bool finite(Vec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
} // namespace

std::optional<LegendsContainerDefinition>
legendsDreamcastContainer(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                          std::string* diagnostic) {
    if (diagnostic)
        diagnostic->clear();
    const bool trapBarrel = info.type == 10 && info.subtype >= 43 && info.subtype <= 45;
    if (info.type != 2 && !trapBarrel)
        return std::nullopt;
    const auto reject = [&](std::string_view reason) -> std::optional<LegendsContainerDefinition> {
        if (diagnostic)
            *diagnostic = reason;
        return std::nullopt;
    };
    const bool barrel = info.subtype == 43 || trapBarrel;
    const bool chest = info.subtype >= 46 && info.subtype <= 48;
    if ((!barrel && !chest) || instance.flags != 0 || info.objectFlags != 0 ||
        info.activeType != (trapBarrel ? 6
                            : barrel   ? 0x206
                                       : 0x16) ||
        info.activeOff != 0 || info.activeOn != 0)
        return reject("Dreamcast container requires unsupported activation or visibility state");
    if (!finite(instance.position) || !finite(instance.rotation) || info.collisionFlags != 0 ||
        info.collisionOffset != Vec3{0} || !std::isfinite(info.height) || info.height <= 0 ||
        info.height > 64)
        return reject("Dreamcast container has unsupported placement or collision bounds");
    if ((barrel && (info.collisionType != 1 || !std::isfinite(info.radius) || info.radius <= 0 ||
                    info.radius > 64 || info.hitPoints <= 0 || info.armor != 1)) ||
        (chest &&
         (info.collisionType != 3 || !std::isfinite(info.xSize) || !std::isfinite(info.zSize) ||
          info.xSize <= 0 || info.zSize <= 0 || info.xSize > 64 || info.zSize > 64)))
        return reject("Dreamcast container has unsupported collision shape or durability");

    LegendsContainerDefinition result;
    result.kind =
        barrel ? LegendsContainerDefinition::Kind::Barrel : LegendsContainerDefinition::Kind::Chest;
    result.nativeIndex = nativeIndex;
    result.position = instance.position;
    // Keep the shared engine's upright body at the authored yaw. The figure
    // retains the full native rotation, including terrain-aligned pitch/roll;
    // neither genuine small tilts nor Euler-export noise disable interaction.
    result.yaw = instance.rotation.y;
    result.closed = instance.name.empty() ? info.name : instance.name;
    result.opening = "ACTIVE";
    result.broken = barrel ? "DONE" : "OPEN";
    result.goldOpening = info.subtype == 48 ? "CHESTSG" : result.closed;
    result.damageThreshold = static_cast<f32>(info.hitPoints);
    result.damageMode = LegendsContainerDefinition::DamageMode::Accumulated;
    result.armor = info.armor;
    result.closedObstacle.centre = instance.position;
    result.closedObstacle.yaw = result.yaw;
    result.closedObstacle.height = info.height;
    // Native collision type 3 compares against xSize/zSize directly: these
    // are half extents, unlike the older N64 PCon full-width fields.
    result.closedObstacle.halfAcross = barrel ? info.radius : info.xSize;
    result.closedObstacle.halfAlong = barrel ? info.radius : info.zSize;
    result.closedObstacle.cylinderRadius = barrel ? info.radius : 0;
    result.closedObstacle.enemyItem = Obstacle::ItemQuery{barrel, info.radius};
    if (trapBarrel && info.subtype != 43) {
        const bool poison = info.subtype == 45;
        result.hazard = poison ? LegendsContainerDefinition::Hazard::Poison
                               : LegendsContainerDefinition::Hazard::Fire;
        // HitItem 8c0afaa0 passes 30 damage and kind 20/21 to 8c0d6d80;
        // that constructor uses 6/8 unit radii and lowers poison's centre 3 units.
        result.blastRadius = poison ? 8.0f : 6.0f;
        result.blastDamage = 30;
        result.blastOffset.y = poison ? -1.0f : 2.0f;
    }
    return result;
}

LegendsDreamcastLoot::LegendsDreamcastLoot(std::span<const formats::ItemInfoRecord> definitions,
                                           u32 seed)
    : m_definitions(definitions.begin(), definitions.end()), m_seed(seed) {}

const formats::ItemInfoRecord& LegendsDreamcastLoot::definition(s16 index) const {
    if (index < 0 || static_cast<usize>(index) >= m_definitions.size())
        throw FormatError("Dreamcast container contents reference an invalid definition");
    return m_definitions[static_cast<usize>(index)];
}

std::vector<s16> LegendsDreamcastLoot::outcomes(s16 root) const {
    std::vector<s16> result;
    if (root < 0)
        return result; // Native 8c0ae502 treats a negative contents row as empty.
    std::vector<u8> visited(m_definitions.size());
    const std::function<void(s16, usize)> visit = [&](s16 index, usize depth) {
        const auto& row = definition(index);
        if (depth > 64 || visited[static_cast<usize>(index)] == 1)
            throw FormatError("Dreamcast container has cyclic or excessive choice nesting");
        if (visited[static_cast<usize>(index)] == 2)
            return;
        visited[static_cast<usize>(index)] = 1;
        if (row.type == formats::ItemInfoRecord::kChoiceList) {
            if (row.choices.empty() || row.choices.size() > formats::ItemInfoRecord::kMostChoices ||
                row.subtype != static_cast<s32>(row.choices.size()))
                throw FormatError("Dreamcast container has invalid native choice count");
            for (const auto choice : row.choices)
                visit(choice, depth + 1);
        } else {
            result.push_back(index);
        }
        visited[static_cast<usize>(index)] = 2;
    };
    visit(root, 0);
    return result;
}

std::optional<s16> LegendsDreamcastLoot::select(s16 root, u32 instanceIndex) {
    if (root < 0)
        return std::nullopt;
    // Validate the graph before consuming any cursor state. Duplicate choices
    // retain their authored weighting, although prebound leaves are unique.
    (void)outcomes(root);
    s16 index = root;
    for (usize depth = 0; depth <= 64; ++depth) {
        const auto& row = definition(index);
        if (row.type != formats::ItemInfoRecord::kChoiceList)
            return index;
        // 8c0a7540 uses a logical shift, instance slot, then native modulo;
        // each nested selector consumes 439, not each leaf or render frame.
        const usize choice = ((m_seed >> 5U) + instanceIndex) % row.choices.size();
        m_seed += 439;
        index = row.choices[choice];
    }
    throw FormatError("Dreamcast container choice depth exceeds budget");
}

void LegendsDreamcastLoot::advance(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0)
        return;
    // Native item loop 8c0ab5ec increments the same cursor once per 30 Hz
    // update. The elapsed fraction remains independent of render cadence.
    m_fraction += static_cast<f64>(seconds) * 30;
    const f64 ticks = std::floor(m_fraction + 1.0e-6);
    m_fraction = std::max(m_fraction - ticks, 0.0);
    m_seed += static_cast<u32>(std::fmod(ticks, 4294967296.0));
}

} // namespace gdl::game
