#include "game/players/MeleeSelection.h"

#include <cmath>
#include <numbers>

#include "engine/math/PortableMath.h"

#include "game/players/PlayerDeeds.h"

namespace gdl::game {
bool MeleeSelection::accepts(const Target& target, const Vec3& facing, f32 range, f32 facingDot) {
    const f32 facingLength = portable::hypot(facing.x, facing.z);
    if (facingLength < 1e-5f || range <= 0) {
        return false;
    }
    const f32 flat = portable::hypot(target.offset.x, target.offset.z);
    const f32 threshold = facingDot + target.score * (1 - facingDot) / range;
    const f32 dot = (target.offset.x * facing.x + target.offset.z * facing.z) / facingLength;
    return dot >= flat * threshold;
}

bool MeleeSelection::accepts(const Contact& contact, const Vec3& facing, f32 range) {
    const auto& target = contact.target;
    const f32 length = glm::length(target.offset);
    const f32 facingLength = glm::length(facing);
    return target.distance < range && facingLength > 0 &&
           ((target.distance <= 0 && !contact.requireFacing) ||
            (length > 0 && glm::dot(target.offset, facing) >= length * facingLength * kHeldCone));
}

std::optional<MeleeSelection::Selection>
MeleeSelection::select(const Snapshot& snapshot, const Vec3& facing, f32 queryReach) {
    if (snapshot.contact && accepts(*snapshot.contact, facing, snapshot.range)) {
        return Selection{true, 0, snapshot.contact->target.distance};
    }
    if (queryReach <= 0) {
        return std::nullopt;
    }
    std::optional<Selection> selected;
    f32 best = snapshot.range;
    for (usize index = 0; index < snapshot.targets.size(); ++index) {
        const auto& target = snapshot.targets[index];
        if (target.score < best && target.distance < queryReach &&
            accepts(target, facing, snapshot.range, snapshot.facingDot)) {
            selected = Selection{false, index, target.distance};
            best = target.score;
        }
    }
    return selected;
}

MeleeSense MeleeSelection::senseOf(const Target& target, const Vec3& feet, f32 reach, f32 yaw,
                                   bool held) {
    MeleeSense sense;
    const f32 bias = held ? kHeldReach : 0;
    sense.range =
        target.distance < reach + kSwingReach + bias ? MeleeRange::Swing : MeleeRange::Step;
    sense.low = target.distance < reach + kStepReach && target.low;
    const Vec3 toward = target.base - feet;
    if (portable::hypot(toward.x, toward.z) > 1e-5f) {
        sense.yaw = std::remainder(portable::atan2(toward.x, toward.z) - yaw,
                                   2.0f * std::numbers::pi_v<f32>);
    }
    return sense;
}

MeleeSense MeleeSelection::sense(const Snapshot& snapshot, bool held, const Vec3& facing, f32 yaw) {
    const f32 reach = snapshot.reach + kStepReach + (held ? kHeldReach : 0);
    const auto selected = select(snapshot, facing, reach);
    if (!selected || selected->distance >= reach) {
        return {MeleeRange::Beyond, false, 0};
    }
    if (selected->contact) {
        if (!snapshot.contact) {
            return {MeleeRange::Beyond, false, 0};
        }
        return senseOf(snapshot.contact->target, snapshot.feet, snapshot.reach, yaw, held);
    }
    return senseOf(snapshot.targets[selected->index], snapshot.feet, snapshot.reach, yaw, held);
}

PlayerDeed MeleeSelection::deed(const MeleeSense& sense, bool strong, bool moved, s32 chain) {
    const bool steps = sense.range == MeleeRange::Step && moved && !sense.low;
    const bool slowInChain = strong && chain != 0 && moved && sense.range != MeleeRange::Beyond;
    if (sense.range != MeleeRange::Swing && !steps && !slowInChain) {
        return strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
    }
    return closeDeedOf(strong ? CloseSwing::Slow : CloseSwing::Quick,
                       sense.low ? CloseHeight::Low : CloseHeight::High);
}
} // namespace gdl::game
