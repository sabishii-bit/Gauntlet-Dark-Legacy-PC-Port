#include "expansions/n64/level/LegendsGates.h"

#include <cmath>
#include <stdexcept>

namespace gdl::game {
namespace {
Obstacle n64Collider(const formats::LegendsN64GateCollider& collider, f32 yaw) {
    Obstacle result;
    result.solid = collider.shape != 5;
    if (!result.solid) {
        return result;
    }
    // e00638b8 stores full box dimensions; e0060d8c/e0060764 halve
    // their fixed-point s16 values. Cylinders use radius directly.
    const auto fixed = [](f32 value) { return std::trunc(value * 32) / 32; };
    const auto half = [](f32 value) { return std::floor(std::trunc(value * 32) / 2) / 32; };
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    result.centre = Vec3{c * collider.offset.x - s * collider.offset.y, 0,
                         s * collider.offset.x + c * collider.offset.y};
    result.yaw = -yaw;
    result.height = fixed(collider.dimensions.y);
    result.halfAcross =
        collider.shape == 1 ? fixed(collider.dimensions.x) : half(collider.dimensions.x);
    result.halfAlong = collider.shape == 1 ? result.halfAcross : half(collider.dimensions.z);
    result.cylinderRadius = collider.shape == 1 ? result.halfAcross : 0;
    return result;
}
} // namespace

LegendsGateDefinition legendsN64Gate(const formats::LegendsN64Gate& native) {
    if (!std::isfinite(native.yaw)) {
        throw std::invalid_argument("Native gate requires a finite placement");
    }
    LegendsGateDefinition result;
    result.nativeIndex = native.nativeIndex;
    result.position = native.position;
    result.primary = n64Collider(native.primary, native.yaw);
    for (usize i = 0; i < result.frames.size(); ++i) {
        result.frames[i] = n64Collider(native.frames[i], native.yaw);
    }
    result.sound = native.sound;
    result.maximumAboveFeet = 3;
    result.initiallyActive = (native.placementFlags & 6U) != 0;
    result.hidden = (native.placementFlags & 8U) != 0;
    return result;
}
} // namespace gdl::game
