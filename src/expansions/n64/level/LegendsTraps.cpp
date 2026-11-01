#include "expansions/n64/level/LegendsTraps.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

f32 legendsTrapDamage(f32 damage, f32 armorAttribute) {
    if (!std::isfinite(damage) || !std::isfinite(armorAttribute) || damage <= 0) {
        return 0;
    }
    // Native armor remains a displayed attribute; e004ba94 rounds the amount
    // absorbed, not the final damage. GDL class stats supply that attribute for
    // imported characters, without changing their ordinary GDL armor rules.
    const f32 fraction = std::min(std::sqrt(std::max(armorAttribute, 0.0f)) * .015f, 1.0f);
    return std::max(0.0f, damage - std::floor(damage * fraction + .5f));
}

LegendsTrap::LegendsTrap(formats::LegendsN64Trap definition, f32 damageScale,
                         u32 activeSequenceFrames, u32 startingSequenceFrames, bool hasEnding)
    : m_definition(std::move(definition)), m_scale(damageScale),
      m_activeFrames(activeSequenceFrames), m_startingFrames(startingSequenceFrames),
      m_hasEnding(hasEnding), m_waiting(static_cast<s32>(m_definition.restFrames) * 5),
      m_random(m_definition.nativeIndex + 1) {
    if (!finite(m_definition.position) || !finite(m_definition.dimensions) ||
        !std::isfinite(m_definition.yaw) || !std::isfinite(damageScale) || damageScale < 0 ||
        damageScale > 100 || activeSequenceFrames == 0 || activeSequenceFrames > 65535 ||
        startingSequenceFrames > 65535 ||
        (m_definition.contactNode.empty() ? (m_definition.shape != 1 && m_definition.shape != 3)
                                          : m_definition.shape != 5) ||
        m_definition.dimensions.x <= 0 ||
        (m_definition.contactNode.empty() ? m_definition.dimensions.y <= 0
                                          : m_definition.dimensions.y != 0) ||
        m_definition.dimensions.z < 0 ||
        (m_definition.shape == 3 && m_definition.dimensions.z == 0)) {
        throw std::invalid_argument("Invalid native damaging trap");
    }
}

Obstacle LegendsTrap::contact() const {
    Obstacle result;
    result.solid = false; // Contact traps never become movement-blocking scenery.
    result.centre = m_definition.position;
    result.yaw = -m_definition.yaw;
    const auto fixed = [](f32 n) { return std::trunc(n * 32) / 32; };
    const auto half = [](f32 n) { return std::floor(std::trunc(n * 32) / 2) / 32; };
    result.height = fixed(m_definition.dimensions.y);
    if (!m_definition.contactNode.empty()) {
        // e00784d8 resolves the animated node's world matrix every tick;
        // e0076d0c places its cylindrical attack from nodeY-3 through nodeY+3.
        const auto point = m_nodeContact.value_or(m_definition.position);
        result.centre = {fixed(point.x), fixed(point.y - 3), fixed(point.z)};
        result.height = 6;
        result.yaw = 0;
    }
    if (m_definition.shape == 1 || !m_definition.contactNode.empty()) {
        result.cylinderRadius = fixed(m_definition.dimensions.x);
        result.halfAcross = result.halfAlong = result.cylinderRadius;
    } else {
        result.halfAcross = half(m_definition.dimensions.x);
        result.halfAlong = half(m_definition.dimensions.z);
    }
    return result;
}

void LegendsTrap::begin() {
    m_phase = m_startingFrames > 0 ? Phase::Starting : Phase::Active;
    const u32 frames = m_definition.activeFrames != 0 ? m_definition.activeFrames
                       : m_startingFrames > 0         ? m_startingFrames
                                                      : m_activeFrames;
    m_waiting = static_cast<s32>(frames) * 5;
    // e0068118 creates one attack at activation, before an optional S sequence.
    // e0079a28 borrows the stationary trap body and e0077b84's bit8000 admits
    // each victim at most once during this attack's independent lifetime.
    m_hazardTicks = m_waiting;
    m_hitPlayers = 0;
    ++m_generation;
}

void LegendsTrap::recover() {
    m_phase = Phase::Idle;
    // The native recovery draws random&127 and adds stage difficulty delay.
    // Keep this offline adapter's random stream local to each trap.
    m_random = m_random * 1664525U + 1013904223U;
    m_waiting = static_cast<s32>(static_cast<f32>(m_definition.restFrames * 5) +
                                 ((100 - m_scale) / 100) * 300 +
                                 static_cast<f32>((m_random >> 16U) & 127U));
    ++m_generation;
}

void LegendsTrap::advance(f32 seconds, bool visible, bool timeStopped, bool animationFinished) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10) {
        return;
    }
    m_stopped = timeStopped;
    if (seconds == 0) {
        return;
    }
    m_accumulator += static_cast<f64>(seconds) * 30;
    while (m_accumulator + 1e-7 >= 1) {
        m_accumulator = std::max(0.0, m_accumulator - 1);
        m_hazardTicks = std::max(0, m_hazardTicks - 5);
        // e0079438/e00784d8 still age the independently created attack while
        // e0068118 suspends its trap state machine under Stop Time. Resuming
        // must not resurrect an expired pulse and hit a newly arriving player.
        if (timeStopped) {
            continue;
        }
        const auto previous = m_phase;
        switch (m_phase) {
        case Phase::Idle:
            if (visible || (m_definition.placementFlags & 6U) != 0) {
                if (m_waiting > 0) {
                    m_waiting -= 5;
                } else {
                    begin();
                }
            }
            break;
        case Phase::Starting:
            if (animationFinished) {
                m_phase = Phase::Active;
                if (m_definition.activeFrames == 0) {
                    m_waiting = static_cast<s32>(m_activeFrames) * 5;
                }
                ++m_generation;
            }
            break;
        case Phase::Active:
            if (m_waiting > 0) {
                m_waiting -= 5;
            } else if (animationFinished) {
                if (m_hasEnding) {
                    m_phase = Phase::Ending;
                    ++m_generation;
                } else {
                    recover();
                }
            }
            break;
        case Phase::Ending:
            if (animationFinished) {
                recover();
            }
            break;
        }
        if (m_phase != previous) {
            // Completion belongs to the old figure, not the newly selected one.
            animationFinished = false;
        }
    }
}

std::optional<LegendsTrapHit> LegendsTrap::touch(const LegendsPortalVisitor& visitor) {
    if (!dangerous() || !visitor.alive || visitor.player >= 4 ||
        (m_hitPlayers & (1U << visitor.player)) != 0 || !finite(visitor.position) ||
        !std::isfinite(visitor.radius) || !std::isfinite(visitor.height) || visitor.radius < 0 ||
        visitor.height < 0) {
        return std::nullopt;
    }
    const auto body = contact();
    if (visitor.position.y > body.centre.y + body.height ||
        visitor.position.y + visitor.height < body.centre.y ||
        !body.touchedBy({visitor.position.x, body.centre.y, visitor.position.z}, visitor.radius)) {
        return std::nullopt;
    }
    m_hitPlayers |= static_cast<u8>(1U << visitor.player);
    LegendsTrapHit result;
    result.nativeIndex = m_definition.nativeIndex;
    result.player = visitor.player;
    result.damage = std::trunc(static_cast<f32>(m_definition.damage) *
                               (m_definition.contactNode.empty() ? m_scale : 1.0f));
    result.pierces = m_definition.templateKey == 0x1001 || m_definition.templateKey == 0x1002;
    result.position = visitor.position;
    result.sound = m_definition.sound;
    // e004ba94 exempts levitating players from category0x60000 trap contacts.
    // Let the shared health dispatcher apply the equivalent immunity.
    result.impact.flags = damage::kLow;
    // Native attack bit1 requests a fall; bit4 requests an ordinary hurt flinch.
    // Translate semantics, never copy the unrelated N64 attack mask into GDL.
    if (!m_definition.contactNode.empty() || (m_definition.flags & 0x40U) != 0) {
        result.impact.flags |= damage::kKnockDown;
        result.impact.direction = visitor.position - m_definition.position;
        result.impact.direction.y = 0;
        if (glm::dot(result.impact.direction, result.impact.direction) < .0001f) {
            result.impact.direction = {std::sin(m_definition.yaw), 0, std::cos(m_definition.yaw)};
        }
    }
    return result;
}

void LegendsTrap::placeAt(const Vec3& feet) {
    if (!finite(feet)) {
        throw std::invalid_argument("Invalid native trap position");
    }
    m_definition.position = feet;
}

void LegendsTrap::nodeContact(std::optional<Vec3> position) {
    if (position && !finite(*position)) {
        throw std::invalid_argument("Invalid native trap attachment");
    }
    m_nodeContact = position;
}
} // namespace gdl::game
