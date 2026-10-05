#include "game/world/TargetAssist.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gdl::game {
f32 TargetAssist::distanceTo(const Vec3& feet, f32 height, const MissileTarget& target) {
    const Vec3 middle = feet + Vec3{0, height * 0.5f, 0};
    return target.surface.empty()
               ? std::hypot(target.base.x - feet.x, target.base.z - feet.z) - target.radius
               : glm::distance(target.pointNear(middle), middle);
}

std::optional<MissileTarget> TargetAssist::around(const Vec3& feet, f32 height,
                                                  std::span<const MissileTarget> targets, f32 reach,
                                                  const WorldCollision* collision) {
    std::optional<MissileTarget> nearest;
    f32 best = reach;
    for (const MissileTarget& target : targets) {
        if (target.id < 0 || target.radius <= 0 || target.height <= 0 ||
            target.base.y >= feet.y + height || target.base.y + target.height <= feet.y) {
            continue;
        }
        const f32 distance = distanceTo(feet, height, target);
        if (distance >= best) {
            continue;
        }
        const Vec3 origin{
            feet.x,
            std::clamp(feet.y + height * 0.5f, target.base.y, target.base.y + target.height),
            feet.z};
        // Facing it, the line to it must be clear of walls.
        const Vec3 toward = target.pointNear(origin) - origin;
        const bool coincident = std::hypot(toward.x, toward.z) < 1e-5f;
        // A melee contact only asks about visibility, not NODE aim preference.
        // Untargetable parts remain hittable at close range.
        MissileTarget sight = target;
        sight.node = -1;
        if (!coincident && !select(origin, Vec3{toward.x, 0.0f, toward.z}, std::span{&sight, 1},
                                   kBossRange, collision)
                                .has_value()) {
            continue;
        }
        nearest = target;
        best = distance;
    }
    return nearest;
}

std::optional<MissileTarget> TargetAssist::ahead(const Vec3& feet, f32 height, const Vec3& facing,
                                                 std::span<const MissileTarget> targets, f32 reach,
                                                 f32 range, const WorldCollision* collision) {
    const f32 facingLength = std::hypot(facing.x, facing.z);
    if (facingLength < 1e-5f || range <= 0 || reach <= 0) {
        return std::nullopt;
    }
    std::vector<MissileTarget> candidates;
    for (const MissileTarget& target : targets) {
        const Vec3 origin{feet.x, feet.y + height * 0.5f, feet.z};
        const Vec3 offset = target.pointNear(origin) - origin;
        const f32 flat = std::hypot(offset.x, offset.z);
        const f32 distance = distanceTo(feet, height, target);
        // closest_enemy / item targeting tighten the forward cone over the
        // full search range, not over this swing's much shorter reach.
        const f32 threshold = kFacingDot + distance * (1 - kFacingDot) / range;
        const f32 dot = (offset.x * facing.x + offset.z * facing.z) / facingLength;
        if (dot >= flat * threshold) {
            candidates.push_back(target);
        }
    }
    return around(feet, height, candidates, reach, collision);
}

std::optional<Vec3> TargetAssist::select(const Vec3& origin, const Vec3& facing,
                                         std::span<const MissileTarget> targets, f32 range,
                                         const WorldCollision* collision) {
    const f32 facingLength = std::hypot(facing.x, facing.z);
    if (facingLength < 1e-5f || range <= 0.0f) {
        return std::nullopt;
    }
    struct Candidate {
        s32 id;
        bool part;
        Vec3 point;
        f32 distance;
        f32 score;
    };
    std::vector<Candidate> candidates;
    for (const MissileTarget& target : targets) {
        if (target.id < 0 || target.radius <= 0.0f || target.height <= 0.0f) {
            continue;
        }
        const Vec3 point = target.pointNear(origin);
        const Vec3 offset = point - origin;
        const f32 flat = std::hypot(offset.x, offset.z);
        const f32 targetRadius = target.surface.empty() ? target.radius : 0.0f;
        const f32 distance = glm::length(offset) - targetRadius;
        if (flat < 1e-5f || distance >= range ||
            (offset.x * facing.x + offset.z * facing.z) / (flat * facingLength) < kFacingDot) {
            continue;
        }
        f32 score = distance;
        const bool part = target.node >= 0;
        if (part) {
            // CritterLineRootColSub chooses a weighted live NODE first. The
            // broad body is only a fallback when no part can be aimed at.
            const f32 length = glm::length(offset);
            const f32 dot = (offset.x * facing.x + offset.z * facing.z) / (length * facingLength);
            const f32 threshold =
                flat / length * (distance * (1 - kFacingDot) / range + kFacingDot);
            if (target.targetScoreScale <= 0 || length > range ||
                (target.maxTargetDistance > 0 && length > target.maxTargetDistance) ||
                dot <= threshold) {
                continue;
            }
            score = distance / (target.targetScoreScale * (dot - threshold));
        }
        // Use missile collision geometry up to the target's near surface: its
        // own world geometry must not hide it from selection.
        bool blocked = false;
        if (collision != nullptr) {
            constexpr f32 kProbeRadius = 0.25f;
            const f32 length = glm::length(offset);
            const Vec3 direction = offset / length;
            const auto steps = static_cast<s32>(std::ceil((length - targetRadius) / kProbeRadius));
            for (s32 step = 1; step < steps; ++step) {
                const f32 along = static_cast<f32>(step) * kProbeRadius;
                const Vec3 at = origin + direction * along;
                if (!target.surface.empty() && target.touches(at, kProbeRadius)) {
                    break;
                }
                if (glm::distance(collision->resolveWalls(at, kProbeRadius, at.y - kProbeRadius,
                                                          at.y + kProbeRadius),
                                  at) > 1e-4f) {
                    blocked = true;
                    break;
                }
            }
        }
        if (!blocked) {
            const Candidate candidate{target.id, part, point, distance, score};
            const auto prior = std::ranges::find(candidates, target.id, &Candidate::id);
            if (prior == candidates.end()) {
                candidates.push_back(candidate);
            } else if ((part && !prior->part) || (part == prior->part && score < prior->score)) {
                *prior = candidate;
            }
        }
    }
    const auto best = std::ranges::min_element(candidates, {}, &Candidate::distance);
    return best != candidates.end() ? std::optional{best->point} : std::nullopt;
}

Vec3 TargetAssist::velocity(const Vec3& origin, const Vec3& target, f32 speed, f32 gravity) {
    const Vec3 offset = target - origin;
    const f32 flight = std::hypot(offset.x, offset.z) / std::max(speed, 1e-5f);
    if (flight < 1e-5f) {
        return Vec3{0};
    }
    return Vec3{offset.x / flight, offset.y / flight + 0.5f * gravity * flight, offset.z / flight};
}
} // namespace gdl::game
