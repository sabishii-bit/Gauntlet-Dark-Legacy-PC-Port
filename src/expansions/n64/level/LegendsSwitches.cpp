#include "expansions/n64/level/LegendsSwitches.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "expansions/n64/level/LegendsPortals.h"

namespace gdl::game {

bool touchesLegendsSwitch(const formats::LegendsN64Switch& definition,
                          const LegendsPortalVisitor& visitor) {
    if (!definition.supported) {
        return false;
    }
    formats::LegendsN64Portal contact;
    contact.position = definition.position;
    contact.radius = definition.radius;
    contact.height = definition.height;
    return touchesLegendsPortal(contact, visitor);
}

f32 LegendsSwitches::Target::sample(f32 alpha) const {
    return alpha < 0 || !std::isfinite(alpha)
               ? offset
               : std::lerp(previous, offset, std::clamp(alpha, 0.0f, 1.0f));
}

LegendsSwitches::LegendsSwitches(std::span<const formats::LegendsN64Switch> definitions,
                                 u8 activationThreshold) {
    for (const auto& definition : definitions) {
        if (!definition.supported || definition.minimumPlayers > activationThreshold) {
            continue;
        }
        if (definition.target.empty() ||
            (definition.mode() != 0x500 && definition.mode() != 0x800 &&
             definition.mode() != 0x200 && definition.mode() != 0x900 &&
             definition.mode() != 0xa00 && definition.mode() != 0x100) ||
            std::ranges::any_of(m_switches, [&](const auto& value) {
                return value.definition.nativeIndex == definition.nativeIndex;
            })) {
            throw std::invalid_argument("Invalid native lift switch definition");
        }
        const bool fading = definition.mode() == 0x100;
        // e006dea0 initializes a new fade record with fixed transparency
        // endpoints. Its existing-record path below still fills zero endpoints
        // from subsequent placements. e006e298 installs the first phase.
        const f32 first = fading ? 1.5f : static_cast<f32>(definition.firstEndpoint) * .1f;
        f32 second = fading ? 0 : static_cast<f32>(definition.secondEndpoint) * .1f;
        if (definition.mode() == 0x500 && second == 0) {
            second = -50; // e006dea0's default descending breakaway endpoint.
        }
        auto target = std::ranges::find(m_targets, definition.target, &Target::name);
        if (target == m_targets.end()) {
            m_targets.push_back({definition.target, first, second, first, first});
            target = std::prev(m_targets.end());
            target->fading = fading;
        } else {
            if (target->fading != fading) {
                throw std::invalid_argument("Conflicting native switch target modes");
            }
            // e006dea0 shares one motion record per WOBJ. A subsequent switch
            // can fill a zero endpoint but cannot replace a nonzero one.
            if (target->first == 0) {
                target->first = first;
                target->offset = first;
                target->previous = first;
            }
            if (target->second == 0) {
                // The native existing-record path does not apply mode500's
                // default again; use the actual second authored parameter.
                target->second = static_cast<f32>(definition.secondEndpoint) * .1f;
            }
        }
        m_switches.push_back({definition, static_cast<usize>(target - m_targets.begin())});
    }
}

void LegendsSwitches::capturePresentation() {
    for (auto& target : m_targets) {
        target.previous = target.offset;
    }
}

bool LegendsSwitches::activate(u32 nativeIndex) {
    const auto plate = std::ranges::find_if(
        m_switches, [&](const auto& value) { return value.definition.nativeIndex == nativeIndex; });
    if (plate == m_switches.end() || plate->activated) {
        return false;
    }
    plate->activated = true;
    request(*plate, 1);
    return true;
}

void LegendsSwitches::request(Switch& plate, u8 occupants) {
    auto& target = m_targets[plate.target];
    switch (plate.definition.mode()) {
    case 0x100:
        target.requested = true;
        target.goal = target.second;
        target.activated = true;
        break;
    case 0x200:
        // e006e2d8 toggles only at rest. A new arriving player bypasses the
        // 450/150-second wait; otherwise repeated contact waits three seconds
        // after the last actual movement, not after the initial press.
        if (!target.moving && ((occupants & static_cast<u8>(~target.occupants)) != 0 ||
                               m_time > target.lastMotion + 3)) {
            target.goal = target.goal == target.second ? target.first : target.second;
            target.activated = true;
        }
        target.occupants = occupants;
        break;
    case 0x900:
        if (target.offset != target.first) {
            target.goal = target.first;
            target.activated = true;
        }
        break;
    case 0xa00:
        if (target.offset != target.second) {
            target.goal = target.second;
            target.activated = true;
        }
        break;
    default:
        target.goal = target.second;
        target.activated = true;
        break;
    }
    target.moving = target.activated && target.offset != target.goal;
}

void LegendsSwitches::contact(u32 nativeIndex, u8 occupants) {
    const auto plate = std::ranges::find_if(
        m_switches, [&](const auto& value) { return value.definition.nativeIndex == nativeIndex; });
    if (plate == m_switches.end()) {
        return;
    }
    occupants &= 15;
    if (plate->definition.oneShot()) {
        if (occupants != 0) {
            activate(nativeIndex);
        }
        return;
    }
    const bool wasActive = plate->activated;
    plate->activated = occupants != 0;
    if (plate->activated || wasActive) {
        // e0068118 state3 clears its occupant field before dispatching. The
        // first idle->active transition alone supplies arriving player bits.
        request(*plate, wasActive ? 0 : occupants);
    }
}

void LegendsSwitches::support(std::span<const usize> targets) {
    for (auto& target : m_targets) {
        target.supported = false;
    }
    for (const usize target : targets) {
        if (target < m_targets.size()) {
            m_targets[target].supported = true;
        }
    }
}

void LegendsSwitches::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 10) {
        return;
    }
    m_time += seconds;
    // e00694e4 initializes every plate idle. Placement flags2/4 request
    // activation on the first real e0068118 tick, independent of contact.
    for (auto& plate : m_switches) {
        if (!plate.activated && (plate.definition.placementFlags & 6U) != 0) {
            activate(plate.definition.nativeIndex);
            plate.definition.placementFlags &= static_cast<u8>(~6U);
        }
    }
    // e006e5dc moves native collision Y at4 units/sec. The renderer uses its
    // separately scaled drawable translation; the bridge owns that conversion.
    const f32 step = seconds * 4;
    for (auto& target : m_targets) {
        if (target.fading && target.activated) {
            // e006e5dc holds a fully revealed bridge while its player mask or
            // enemy support count is nonzero. Contact from either pad is ORed
            // for the frame; an idle sibling must not cancel an active one.
            target.goal = target.requested || (target.offset == target.second && target.supported)
                              ? target.second
                              : target.first;
            target.moving = target.offset != target.goal;
            target.requested = false;
        }
        if (!target.activated || !target.moving) {
            continue;
        }
        target.offset += std::clamp(target.goal - target.offset, -step, step);
        target.lastMotion = m_time;
        target.moving = target.offset != target.goal;
    }
}

} // namespace gdl::game
