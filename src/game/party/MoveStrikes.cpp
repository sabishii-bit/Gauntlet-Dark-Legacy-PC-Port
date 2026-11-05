#include "game/party/MoveStrikes.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/combat/StrikeHit.h"

namespace gdl::game {
bool MoveStrikes::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("MoveStrikes.native.1");
    digest.value(m_sim);
    return digest.good();
}

bool MoveStrikes::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("MoveStrikes.native.1");
    digest.value(saved.m_state);
    return digest.good();
}

void MoveStrikes::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_lifetime = m_restoreOwner;
    saved.m_epoch = m_localEpoch;
}

bool MoveStrikes::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto lifetime = saved.m_lifetime.lock();
    return saved.m_owner == this && lifetime && lifetime == m_restoreOwner &&
           saved.m_epoch == m_localEpoch;
}

bool MoveStrikes::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
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
                       const Vec3& facing, f32 ownDamage, f32 effectSeconds) {
    Strike started;
    started.id = m_sim.next++;
    started.owner = owner;
    started.flies = strike.type == MoveStrike::kFlies;
    started.expanding = strike.type == MoveStrike::kBursts;
    constexpr s32 kIgnoresWorld = 0x40;
    started.collidesWorld = (strike.flags & kIgnoresWorld) == 0;
    started.position = originOf(strike, position, facing);
    const f32 heading = portable::atan2(facing.x, facing.z) + strike.angle;
    started.facing = Vec3{portable::sin(heading), 0, portable::cos(heading)};
    started.speed = strike.speed;
    started.radius = started.flies ? strike.hitRadius : strike.radius;
    started.arc = started.flies ? -1.0f : strike.arc;
    started.damage = damageOf(strike, ownDamage);
    started.delayLeft = strike.delay;
    started.secondsLeft = started.flies ? strike.maxTime : effectSeconds;
    if (started.flies && strike.loopEffect >= 0) {
        // ProcessEffects changes the birth tree on its last native tick. Its
        // morph then gets a fresh maxTime, rather than spending it on the birth.
        constexpr f32 kMorphLead = 0.03332f;
        const f32 birth = std::max(effectSeconds - kMorphLead, 0.0f);
        started.secondsLeft += birth;
        if ((strike.flags & MoveStrike::kWaitsForMorph) != 0) {
            started.launchIn = birth;
        }
    }
    started.damageTime = effectSeconds - strike.delay;
    m_sim.strikes.push_back(started);
    return started.id;
}

std::vector<StrikeHit> MoveStrikes::update(f32 seconds, const WorldCollision* collision) {
    std::vector<StrikeHit> hits;
    if (seconds <= 0) {
        return hits;
    }
    for (Strike& strike : m_sim.strikes) {
        const Vec3 from = strike.position;
        const auto hit = [&] {
            return StrikeHit{strike.id,     strike.owner, strike.position,
                             strike.radius, strike.arc,   strike.facing,
                             strike.damage, from,         strike.flies};
        };
        if (!strike.flies) {
            // Sample long updates too, so crossing the entire active window does not
            // erase the wave. The effect's last third is visual recovery only.
            constexpr f32 kFrame = 1.0f / 30.0f;
            constexpr f32 kTail = 0.33f;
            f32 left = seconds;
            while (left > 0 && strike.secondsLeft > 0) {
                const f32 step = std::min(left, kFrame);
                left -= step;
                strike.secondsLeft -= step;
                if (strike.secondsLeft <= 0) {
                    break;
                }
                StrikeHit area = hit();
                if (strike.expanding) {
                    if (strike.secondsLeft > strike.damageTime) {
                        continue;
                    }
                    const f32 phase =
                        strike.damageTime <= kFrame ? 1.0f : strike.secondsLeft / strike.damageTime;
                    if (phase <= kTail) {
                        continue;
                    }
                    area.radius *= 1.0f + kTail - phase;
                    area.damage *= 1.5f * (phase - kTail);
                }
                area.hitGap = strike.expanding ? strike.secondsLeft + 0.066667f
                                               : std::min(1.0f, strike.secondsLeft);
                hits.push_back(area);
            }
            continue;
        }
        strike.delayLeft = std::max(strike.delayLeft - seconds, 0.0f);
        const f32 liveTime = std::clamp(strike.secondsLeft, 0.0f, seconds);
        const f32 heldTime = std::min(strike.launchIn, liveTime);
        strike.launchIn -= heldTime;
        const f32 travelTime = liveTime - heldTime;
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
        if (liveTime > 0 && strike.delayLeft <= 0.0f && strike.damage > 0.0f) {
            hits.push_back(hit());
        }
    }
    std::erase_if(m_sim.strikes, [](const Strike& strike) { return strike.secondsLeft <= 0.0f; });
    return hits;
}

void MoveStrikes::placeArea(u32 id, const Mat4& placement) {
    const auto found = std::ranges::find(m_sim.strikes, id, &Strike::id);
    if (found == m_sim.strikes.end() || found->flies) {
        return;
    }
    found->position = Vec3{placement[3]};
    const Vec3 forward{placement[2].x, 0, placement[2].z};
    const f32 length = glm::length(forward);
    if (length > 0) {
        found->facing = forward / length;
    }
}

void MoveStrikes::stop(u32 id) {
    std::erase_if(m_sim.strikes, [id](const Strike& strike) { return strike.id == id; });
}

void MoveStrikes::clear() {
    ++m_localEpoch;
    m_sim.strikes.clear();
    m_sim.next = 1;
}

void MoveStrikes::hitPlayer(u32 id, bool reflected, const Vec3& from) {
    const auto found = std::ranges::find(m_sim.strikes, id, &Strike::id);
    if (found == m_sim.strikes.end() || !found->flies) {
        return;
    }
    if (!reflected) {
        m_sim.strikes.erase(found);
        return;
    }
    constexpr f32 kMostLife = 10.0f;
    constexpr f32 kLifeLost = 1.0f;
    constexpr f32 kMostDamage = 15.0f;
    found->position = from;
    found->facing = -found->facing;
    found->secondsLeft =
        found->secondsLeft > kMostLife ? kMostLife : found->secondsLeft - kLifeLost;
    found->damage = std::min(found->damage, kMostDamage);
}

const MoveStrikes::Strike* MoveStrikes::find(u32 id) const {
    const auto found = std::ranges::find(m_sim.strikes, id, &Strike::id);
    return found != m_sim.strikes.end() ? &*found : nullptr;
}

} // namespace gdl::game
