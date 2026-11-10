#include "expansions/dreamcast/level/LegendsDreamcastGates.h"

#include <cmath>

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

std::optional<LegendsGateDefinition>
legendsDreamcastGate(const formats::ItemInfoRecord& info,
                     const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                     std::string* diagnostic) {
    if (diagnostic) {
        diagnostic->clear();
    }
    if (info.type != 7) {
        return std::nullopt;
    }
    const auto reject = [&](std::string_view reason) -> std::optional<LegendsGateDefinition> {
        if (diagnostic) {
            *diagnostic = reason;
        }
        return std::nullopt;
    };
    // SetItem 8c0a8340 creates type 7 with the ordinary item animation state
    // machine. The shipped key doors are active 0x46: CLOSE -> ACTIV -> OPEN.
    // D2's column doors instead use triangle lists and script activation 0x06.
    if (info.collisionType != 3 || info.activeType != 0x46) {
        return reject("Dreamcast gate needs unsupported triangle or scripted activation");
    }
    if (info.subtype < 0 || info.subtype > 3 || instance.flags != 0 || info.objectFlags != 0 ||
        info.activeOff != 0 || info.activeOn != 0 || info.collisionFlags != 0 ||
        info.collisionOffset != Vec3{0}) {
        return reject("Dreamcast gate has unsupported activation or collision flags");
    }
    // The editor exports near-zero Euler noise in otherwise upright gates.
    if (!finite(instance.position) || !finite(instance.rotation) ||
        std::abs(instance.rotation.x) > .00001f || std::abs(instance.rotation.z) > .00001f ||
        !std::isfinite(info.height) || !std::isfinite(info.xSize) || !std::isfinite(info.zSize) ||
        !std::isfinite(info.radius) || info.height <= 0 || info.height > 64 || info.xSize <= 0 ||
        info.xSize > 64 || info.zSize <= 0 || info.zSize > 64 || info.radius <= 0 ||
        info.radius > 128) {
        return reject("Dreamcast gate has invalid or unsupported placement dimensions");
    }

    LegendsGateDefinition result;
    result.nativeIndex = nativeIndex;
    result.position = instance.position;
    // Native box collision uses the exported half extents without N64's
    // full-width halving/1:32 quantization. Dreamcast's native item matrix
    // preserves the authored yaw sign (InitItems 8c0a9b20 -> 8c0ece60).
    result.primary.solid = true;
    result.primary.yaw = instance.rotation.y;
    result.primary.halfAcross = info.xSize;
    result.primary.halfAlong = info.zSize;
    result.primary.height = info.height;
    result.primary.enemyItem = Obstacle::ItemQuery{false, info.radius};
    // ItemTouch 8c0af0e0 case 7 + key debit 8c0b00e0. Unlike N64's extra
    // three-unit foot check, Dreamcast relies on ordinary volume contact.
    result.rejectRetreat = true;
    // 8c0e0c40 uses a Dreamcast realm/subtype sound table. Do not pass its
    // IDs into the N64 macro bank; native Dreamcast effect audio is separate.
    return result;
}

} // namespace gdl::game
