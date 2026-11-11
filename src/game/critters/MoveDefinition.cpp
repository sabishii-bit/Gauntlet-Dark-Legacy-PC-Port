#include "game/critters/MoveDefinition.h"

#include <algorithm>
#include <cmath>

#include "engine/math/PortableMath.h"
namespace gdl::game {
bool TargetCriteria::allows(f32 distance, f32 bearing, f32 vertical) const {
    if (distance < minDistance) {
        return false;
    }
    if (maxDistance > 0.0f && distance > maxDistance) {
        return false;
    }
    if (portable::cos(bearing - yaw) < minDot) {
        return false;
    }
    return maxVertical <= 0.0f || std::abs(vertical) <= maxVertical;
}

bool TargetCriteria::allowsPhase(f32 rateScale) const {
    // CritterCalcTarget: a positive upper bound is exclusive whatever the lower; a band that
    // closes on itself admits nothing, so such a move comes only by its pattern or link.
    return rateScale >= minRateScale && (maxRateScale <= 0.0f || rateScale < maxRateScale);
}

bool TargetCriteria::allowsFromHome(f32 homeDistance) const {
    // Only CritterGetTargetSub tests it (0x80036a84-0x80036aa4; DC 0x8c08d128-0x8c08d13a):
    // CritterCalcTarget, which sight and the step search use, never does.
    return maxHomeDistance <= 0.0f || homeDistance <= maxHomeDistance;
}

bool MoveDefinition::interrupts(const MoveDefinition& current) const {
    if (priority >= kCutsIn && current.interrupt != 0) {
        return true;
    }
    switch (current.interrupt) {
    case 0: return false;
    case 20: return (priority & ~0xFF) > (current.priority & ~0xFF);
    case 60: return priority >= current.priority;
    case 80: return priority > 0;
    case 90: return true;
    default: return priority > current.priority;
    }
}

/** The body's way turned by the record's yaw, then tipped by its pitch (under nought, up),
 * at its speed. */
Vec3 AttackDefinition::spewVelocity(f32 bodyYaw) const {
    const f32 heading = bodyYaw + yaw;
    const f32 level = portable::cos(-pitch);
    return Vec3{portable::sin(heading) * level, portable::sin(-pitch),
                portable::cos(heading) * level} *
           speed;
}

f32 AttackDefinition::spewHalfAngle() const {
    return portable::acos(std::clamp(minDot, -1.0f, 1.0f));
}

s32 MoveDefinition::projectileTriggers(s32 previous, s32 current, bool second) const {
    const s32 first = second ? frameStart2 : frameStart;
    if (first < 0 || current < first || current <= previous) {
        return 0;
    }
    constexpr s32 kRepeatedProjectile = 133;
    if (type != kRepeatedProjectile) {
        return previous < first ? 1 : 0;
    }
    const s32 last = second ? frameEnd2 : frameEnd;
    s32 count = 0;
    for (s32 frame = std::max(first, previous + 1); frame <= std::min(last, current); ++frame) {
        if (framePeriod <= 0.0f ||
            static_cast<s32>(std::fmod(static_cast<f32>(frame - first), framePeriod)) == 0) {
            ++count;
        }
    }
    return count;
}

std::string CombatEffectDefinition::soundFor(char letter) const {
    std::string name = soundFormat;
    if (const auto at = name.find("%c"); at != std::string::npos) {
        name.replace(at, 2, 1, letter);
    }
    return name;
}

} // namespace gdl::game
