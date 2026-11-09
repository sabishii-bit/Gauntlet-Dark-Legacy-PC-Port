#include "expansions/n64/level/LegendsGates.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
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

LegendsGateDefinition n64Definition(const formats::LegendsN64Gate& native) {
    if (!std::isfinite(native.yaw))
        throw std::invalid_argument("Native gate requires a finite placement");
    LegendsGateDefinition result;
    result.nativeIndex = native.nativeIndex;
    result.position = native.position;
    result.primary = n64Collider(native.primary, native.yaw);
    for (usize i = 0; i < result.frames.size(); ++i)
        result.frames[i] = n64Collider(native.frames[i], native.yaw);
    result.sound = native.sound;
    result.maximumAboveFeet = 3;
    result.initiallyActive = (native.placementFlags & 6U) != 0;
    result.hidden = (native.placementFlags & 8U) != 0;
    return result;
}
} // namespace

LegendsGate::LegendsGate(LegendsGateDefinition definition) : m_definition(std::move(definition)) {
    if (!finite(m_definition.position))
        throw std::invalid_argument("Native gate requires a finite placement");
}

LegendsGate::LegendsGate(formats::LegendsN64Gate definition)
    : LegendsGate(n64Definition(definition)) {}

Obstacle LegendsGate::obstacle(Obstacle collider) const {
    if (collider.solid)
        collider.centre += m_definition.position;
    return collider;
}

Obstacle LegendsGate::primaryObstacle() const {
    auto result = obstacle(m_definition.primary);
    result.solid &= m_state == State::Closed;
    return result;
}

std::array<Obstacle, 2> LegendsGate::frameObstacles() const {
    return {obstacle(m_definition.frames[0]), obstacle(m_definition.frames[1])};
}

bool LegendsGate::touch(const LegendsPortalVisitor& visitor, s32& keys) {
    // Dreamcast ItemTouch (8c0af0e0 case7) does not spend a key while
    // backing away. Retain actual fixed-tick positions, not render poses.
    bool retreating = false;
    if (m_definition.rejectRetreat && visitor.player < m_previousVisitors.size() &&
        finite(visitor.position)) {
        auto& previous = m_previousVisitors[visitor.player];
        if (previous) {
            const Vec3 step = visitor.position - *previous;
            const Vec3 toward = m_definition.position - visitor.position;
            retreating = step.x * toward.x + step.z * toward.z < 0;
        }
        previous = visitor.position;
    }
    if (m_state != State::Closed || keys <= 0 || !visitor.alive || visitor.player >= 4 ||
        !finite(visitor.position) || !std::isfinite(visitor.radius) || visitor.radius < 0 ||
        !std::isfinite(visitor.height) || visitor.height < 0 || retreating) {
        return false;
    }
    const auto body = primaryObstacle();
    // e0069c90 additionally rejects contact more than3 units above the gate's
    // feet. Use feet here, not the port's deliberately higher camera follow point.
    if (!body.solid ||
        visitor.position.y - m_definition.position.y > m_definition.maximumAboveFeet ||
        visitor.position.y > body.centre.y + body.height ||
        visitor.position.y + visitor.height < body.centre.y ||
        !body.touchedBy({visitor.position.x, body.centre.y, visitor.position.z}, visitor.radius,
                        1.0f / 32)) {
        return false;
    }
    --keys;
    m_state = State::Opening;
    return true;
}

bool LegendsGate::finishOpening() {
    if (m_state != State::Opening) {
        return false;
    }
    m_state = State::Open;
    return true;
}

bool LegendsGate::activateInitially() {
    if (m_state != State::Closed || !m_definition.initiallyActive) {
        return false;
    }
    m_state = State::Opening;
    return true;
}

void LegendsGate::placeAt(const Vec3& feet) {
    if (!finite(feet)) {
        throw std::invalid_argument("Native gate requires a finite placement");
    }
    m_definition.position = feet;
}
} // namespace gdl::game
