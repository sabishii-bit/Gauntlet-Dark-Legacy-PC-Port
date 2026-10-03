#include "game/players/ComboMove.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

/** The horizontal length of a vector. */
f32 groundLength(const Vec3& v) {
    return std::hypot(v.x, v.z);
}

/** The two are no longer tied: neither holds, rides or flies. */
void unlink(ComboState& grabber, ComboState& partner) {
    ComboMove::clear(grabber);
    ComboMove::clear(partner);
}

} // namespace

std::optional<usize> ComboMove::findPartner(usize self, const Vec3& position, const Vec3& facing,
                                            std::span<const Candidate> candidates) {
    const f32 length = groundLength(facing);
    if (length <= 0.0f) {
        return std::nullopt;
    }
    const Vec3 ahead{facing.x / length, 0.0f, facing.z / length};
    std::optional<usize> closest;
    f32 best = kReach;
    for (usize i = 0; i < candidates.size(); ++i) {
        if (i == self || !candidates[i].eligible) {
            continue;
        }
        const Vec3 gap = candidates[i].position - position;
        if (std::abs(gap.y) > kRise) {
            continue;
        }
        const f32 distance = groundLength(gap);
        if (distance > best || distance <= 0.0f) {
            continue;
        }
        if ((gap.x * ahead.x + gap.z * ahead.z) / distance < kCone) {
            continue;
        }
        closest = i;
        best = distance;
    }
    return closest;
}

void ComboMove::link(ComboState& grabber, usize grabberIndex, ComboState& partner,
                     usize partnerIndex, s32 grabberClass) {
    clear(grabber);
    clear(partner);
    grabber.role = ComboRole::Grabber;
    grabber.partner = static_cast<s32>(partnerIndex);
    grabber.grabberClass = grabberClass;
    partner.role = ComboRole::Held;
    partner.partner = static_cast<s32>(grabberIndex);
    partner.grabberClass = grabberClass;
}

ComboOrders ComboMove::advance(ComboState& grabber, ComboState& partner, const ComboPhase& phase,
                               s32 ticks) {
    ComboOrders orders;
    if (grabber.role != ComboRole::Grabber || !partner.active()) {
        return orders;
    }
    grabber.ticksLeft = std::max(grabber.ticksLeft - ticks, 0);
    const bool holding = phase.act1 || phase.act2;
    const auto letGo = [&] {
        orders.unlink = true;
        unlink(grabber, partner);
    };
    switch (grabber.grabberClass) {
    case kWizard:
    case kKnight:
    case kSorceress:
    case kJester:
        // The partner is lifted for the move; the jester only holds it for a frame.
        if (holding && !partner.riding) {
            orders.attach = ComboOrders::Attach::PartnerOnGrabber;
            partner.riding = true;
            break;
        }
        if (holding && grabber.grabberClass != kJester) {
            break;
        }
        if (partner.role == ComboRole::Held) {
            orders.restorePartner = partner.riding;
            partner.riding = false;
            letGo();
        }
        break;
    case kValkyrie:
    case kArcher:
        // The grabber is lifted by its partner, who turns its back to it.
        if (holding) {
            if (!grabber.riding) {
                orders.turnPartnerAway = true;
                orders.attach = ComboOrders::Attach::GrabberOnPartner;
                grabber.riding = true;
            }
        } else if (partner.role == ComboRole::Held) {
            orders.restoreGrabber = grabber.riding;
            grabber.riding = false;
            letGo();
        }
        break;
    case kWarrior:
        // Held up to the thirtieth frame, then let fly for four seconds.
        if (phase.act1 && phase.frame < kThrowFrame) {
            if (!partner.riding) {
                orders.attach = ComboOrders::Attach::PartnerOnGrabber;
                partner.riding = true;
                partner.graceSeconds = kThrowerGrace;
            }
            break;
        }
        if (partner.role == ComboRole::Held) {
            orders.releasePartner = partner.riding;
            partner.riding = false;
            partner.role = ComboRole::Thrown;
            grabber.ticksLeft = kFlightTicks;
        }
        if (partner.role == ComboRole::Thrown && grabber.ticksLeft <= 0) {
            letGo();
        }
        break;
    case kDwarf:
        // On the partner's back through COMBOACT1, then steering it for four seconds.
        if (phase.act1) {
            if (!grabber.riding) {
                orders.turnPartnerAway = true;
                orders.attach = ComboOrders::Attach::GrabberOnPartner;
                grabber.riding = true;
                grabber.rideAsked = true;
                partner.graceSeconds = kThrowerGrace;
            }
            grabber.ticksLeft = kFlightTicks;
            break;
        }
        if (partner.role == ComboRole::Held) {
            partner.role = ComboRole::Thrown;
        } else if (partner.role == ComboRole::Thrown && grabber.ticksLeft <= 0) {
            orders.detachGrabber = grabber.riding;
            grabber.riding = false;
            grabber.rideAsked = false;
            letGo();
        }
        break;
    default:
        // A class with no move of its own lets go at once.
        orders.restorePartner = partner.riding;
        orders.restoreGrabber = grabber.riding;
        partner.riding = false;
        grabber.riding = false;
        letGo();
        break;
    }
    return orders;
}

f32 ComboMove::bounceYaw(f32 yaw) {
    return std::remainder(yaw + kBounceTurn, 2.0f * std::numbers::pi_v<f32>);
}

f32 ComboMove::reflectedYaw(f32 yaw, const Vec3& normal) {
    const Vec3 facing{std::sin(yaw), 0.0f, std::cos(yaw)};
    const f32 length = groundLength(normal);
    if (length <= 0.0f) {
        return bounceYaw(yaw);
    }
    const Vec3 n{normal.x / length, 0.0f, normal.z / length};
    const f32 along = facing.x * n.x + facing.z * n.z;
    const Vec3 reflected = facing - n * (2.0f * along);
    return std::atan2(reflected.x, reflected.z);
}

bool ComboMove::takeTurn(ComboState& flier) {
    if (flier.turnTicks > 0) {
        return false;
    }
    flier.turnTicks = kTurnGap;
    return true;
}

void ComboMove::tick(ComboState& flier, s32 ticks, f32 seconds) {
    flier.turnTicks = std::max(flier.turnTicks - ticks, 0);
    flier.graceSeconds = std::max(flier.graceSeconds - seconds, 0.0f);
}

} // namespace gdl::game
