#include "game/world/MoveStrikes.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

bool StrikeHit::reaches(const Vec3& position, f32 targetRadius, f32 targetHeight) const {
    if (radius <= 0 || damage <= 0) {
        return false;
    }
    Vec3 nearest = centre;
    if (swept) {
        const Vec3 travel = centre - from;
        const f32 lengthSquared = travel.x * travel.x + travel.z * travel.z;
        if (lengthSquared > 0) {
            const Vec3 toward = position - from;
            const f32 along =
                std::clamp((toward.x * travel.x + toward.z * travel.z) / lengthSquared, 0.0f, 1.0f);
            nearest = from + travel * along;
        }
    }
    const Vec3 offset = position - nearest;
    const f32 across = std::hypot(offset.x, offset.z);
    if (across > radius + targetRadius || offset.y > radius || offset.y + targetHeight < -radius) {
        return false;
    }
    if (arc <= -1.0f || across < 1e-4f) {
        return true;
    }
    return (offset.x * facing.x + offset.z * facing.z) / across >= arc;
}

f32 MoveStrikes::damageOf(const MoveStrike& strike, f32 ownDamage) {
    return strike.amount < 0.0f ? ownDamage * -strike.amount : strike.amount;
}

Vec3 MoveStrikes::originOf(const MoveStrike& strike, const Vec3& position, const Vec3& facing) {
    // The body's z is its facing and its x lies to that side of it.
    const Vec3 side{facing.z, 0.0f, -facing.x};
    return position + side * strike.offset.x + Vec3{0.0f, strike.offset.y, 0.0f} +
           facing * strike.offset.z;
}

u32 MoveStrikes::start(const MoveStrike& strike, s32 owner, const Vec3& position,
                       const Vec3& facing, f32 ownDamage) {
    Strike started;
    started.id = m_next++;
    started.owner = owner;
    started.flies = strike.type == MoveStrike::kFlies;
    constexpr s32 kIgnoresWorld = 0x40;
    started.collidesWorld = (strike.flags & kIgnoresWorld) == 0;
    started.position = originOf(strike, position, facing);
    const f32 heading = std::atan2(facing.x, facing.z) + strike.angle;
    started.facing = Vec3{std::sin(heading), 0, std::cos(heading)};
    started.speed = strike.speed;
    started.radius = started.flies ? strike.hitRadius : strike.radius;
    started.arc = started.flies ? -1.0f : strike.arc;
    started.damage = damageOf(strike, ownDamage);
    started.delayLeft = strike.delay;
    started.secondsLeft = strike.maxTime;
    m_strikes.push_back(started);
    return started.id;
}

std::vector<StrikeHit> MoveStrikes::update(f32 seconds, const WorldCollision* collision) {
    std::vector<StrikeHit> hits;
    if (seconds <= 0) {
        return hits;
    }
    for (Strike& strike : m_strikes) {
        const Vec3 from = strike.position;
        const auto hit = [&] {
            return StrikeHit{strike.id,     strike.owner, strike.position,
                             strike.radius, strike.arc,   strike.facing,
                             strike.damage, from,         strike.flies};
        };
        if (!strike.flies) {
            strike.delayLeft -= seconds;
            if (strike.delayLeft <= 0.0f) {
                hits.push_back(hit());
                strike.secondsLeft = 0.0f;
            }
            continue;
        }
        strike.delayLeft = std::max(strike.delayLeft - seconds, 0.0f);
        const f32 travelTime = std::clamp(strike.secondsLeft, 0.0f, seconds);
        strike.secondsLeft -= seconds;
        const f32 travel = strike.speed * travelTime;
        constexpr f32 kWallStep = 0.25f;
        const auto steps = collision != nullptr && strike.collidesWorld
                               ? std::max(1, static_cast<s32>(std::ceil(travel / kWallStep)))
                               : 1;
        for (s32 step = 0; step < steps; ++step) {
            const Vec3 next = strike.position + strike.facing * (travel / static_cast<f32>(steps));
            if (collision != nullptr && strike.collidesWorld) {
                const Vec3 pushed = collision->resolveWalls(next, 0.5f, next.y, next.y + 1.0f);
                if (glm::distance(pushed, next) > 1e-3f) {
                    strike.secondsLeft = 0.0f;
                    break;
                }
            }
            strike.position = next;
        }
        if (travelTime > 0 && strike.delayLeft <= 0.0f && strike.damage > 0.0f) {
            hits.push_back(hit());
        }
    }
    std::erase_if(m_strikes, [](const Strike& strike) {
        return strike.flies ? strike.secondsLeft <= 0.0f
                            : strike.delayLeft <= 0.0f && strike.secondsLeft <= 0.0f;
    });
    return hits;
}

void MoveStrikes::clear() {
    m_strikes.clear();
    m_next = 1;
}

const MoveStrikes::Strike* MoveStrikes::find(u32 id) const {
    const auto found = std::ranges::find(m_strikes, id, &Strike::id);
    return found != m_strikes.end() ? &*found : nullptr;
}

} // namespace gdl::game
