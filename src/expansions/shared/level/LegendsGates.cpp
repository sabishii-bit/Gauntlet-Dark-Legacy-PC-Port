#include "expansions/shared/level/LegendsGates.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
} // namespace

LegendsGate::LegendsGate(LegendsGateDefinition definition) : m_definition(std::move(definition)) {
    if (!finite(m_definition.position)) {
        throw std::invalid_argument("Native gate requires a finite placement");
    }
}

Obstacle LegendsGate::obstacle(Obstacle collider) const {
    if (collider.solid) {
        collider.centre += m_definition.position;
    }
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
    // Dreamcast ItemTouch (8c0af0e0 case 7) does not spend a key while
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
    // N64 e0069c90 additionally rejects contact more than 3 units above the gate's
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
