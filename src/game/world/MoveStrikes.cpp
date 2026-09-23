#include "game/world/MoveStrikes.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

bool StrikeHit::reaches(const Vec3& position, f32 targetRadius, f32 targetHeight) const {
    const Vec3 offset = position - centre;
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
    started.flies = strike.type == MoveStrike::kFlies && strike.speed > 0.0f;
    started.position = originOf(strike, position, facing);
    started.facing = facing;
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
    for (Strike& strike : m_strikes) {
        const auto hit = [&] {
            return StrikeHit{strike.id,  strike.owner,  strike.position, strike.radius,
                             strike.arc, strike.facing, strike.damage};
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
        strike.secondsLeft -= seconds;
        strike.position += strike.facing * (strike.speed * seconds);
        if (collision != nullptr) {
            const Vec3 pushed = collision->resolveWalls(strike.position, 0.5f, strike.position.y,
                                                        strike.position.y + 1.0f);
            if (glm::distance(pushed, strike.position) > 1e-3f) {
                strike.secondsLeft = 0.0f; // a wall ends it
            }
        }
        if (strike.delayLeft <= 0.0f && strike.damage > 0.0f) {
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
