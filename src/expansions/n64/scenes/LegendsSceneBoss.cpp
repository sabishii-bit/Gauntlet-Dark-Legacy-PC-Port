#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64BossRewards(RenderDevice& device,
                                             const LegendsBossEncounter& encounter,
                                             std::span<const formats::LegendsN64Fixture> fixtures) {
    if (!m_bossRewards.empty() || fixtures.size() != encounter.rewards.size() ||
        fixtures.size() > 4) {
        throw FormatError("N64 Legends boss: inconsistent reward binding");
    }
    for (usize i = 0; i < fixtures.size(); ++i) {
        std::string diagnostic;
        auto pickup = legendsN64Pickup(fixtures[i], &diagnostic, m_playerCount);
        if (!pickup) {
            if (!diagnostic.empty()) {
                m_warnings.push_back(std::move(diagnostic));
            }
            continue;
        }
        ItemInstance placement;
        placement.position = fixtures[i].position;
        const usize figure = m_fixtures.size();
        place(device, formats::legendsN64TreeName(fixtures[i].group, fixtures[i].sequence),
              placement);
        if (m_fixtures.size() == figure) {
            // Never leave an invisible collectible if its native bank is absent.
            continue;
        }
        pickup->active = false;
        m_hiddenFixtures[figure] = true;
        m_bossRewards.push_back({encounter.rewards[i], m_pickups.size(), figure});
        m_pickups.push_back(std::move(*pickup));
        m_pickupFigures.push_back(figure);
    }
}

void LegendsSceneAdapter::spawnBossRewards(Vec3 death, Vec3 start) {
    if (m_bossRewardsReleased) {
        return;
    }
    // Invalid callbacks must not consume the one-shot release latch.
    for (const auto& reward : m_bossRewards) {
        if (!legendsBossRewardMotion(reward.definition, death, start)) {
            return;
        }
    }
    m_bossRewardsReleased = true;
    for (auto& reward : m_bossRewards) {
        reward.motion = *legendsBossRewardMotion(reward.definition, death, start);
        reward.flying = true;
        reward.released = true;
        auto& pickup = m_pickups[reward.pickup];
        pickup.active = true;
        pickup.position = reward.motion.position;
        auto& figure = m_fixtures[reward.figure];
        figure.placeAt(glm::translate(Mat4{1}, pickup.position));
        const auto* animation = figure.sequenceInfo(0);
        figure.play(0, animation != nullptr && animation->repeats);
        figure.capturePresentation();
        m_hiddenFixtures[reward.figure] = false;
    }
}

void LegendsSceneAdapter::updateBossRewards(f32 seconds, const WorldCollision& collision) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 1) {
        return;
    }
    for (auto& reward : m_bossRewards) {
        auto& pickup = m_pickups[reward.pickup];
        if (!reward.flying || pickup.collected) {
            continue;
        }
        // e00784d8 advances position before applying twice the gravity byte
        // (0x20). Bound substeps keep a long application frame from tunnelling.
        f32 remaining = seconds;
        while (remaining > 0 && reward.flying) {
            const f32 step = std::min(remaining, 1.0f / 150.0f);
            remaining -= step;
            const Vec3 previous = reward.motion.position;
            Vec3 next = previous + reward.motion.velocity * step;
            reward.motion.velocity.y -= 64 * step;
            reward.motion.elapsed += step;
            // Native projectile sphere is0.5, world query uses half its radius.
            // The common query covers both floor and wall triangles.
            const auto hit =
                collision.sweepSphere(previous, next, 0.25f, WorldCollision::kMissileQueryFlags);
            const auto floor = collision.floorAt(next, std::max(previous.y - next.y, 0.0f), 0);
            const bool landed =
                reward.motion.velocity.y <= 0 && floor && next.y <= floor->y + 0.25f;
            if (hit || landed || reward.motion.elapsed >= 5) {
                if (hit) {
                    next = glm::mix(previous, next, hit->time);
                }
                // e00784d8 case10 calls e0064714 then adds one unit. That
                // helper first probes1.5 above/15 below, then retries one
                // unit higher up to29; e003fee0 adds the native0.2 floor pad.
                auto resting = collision.floorAt(next, 1.5f, 15, 1);
                for (s32 rise = 0; !resting && rise < 30; ++rise) {
                    resting =
                        collision.floorAt(next + Vec3{0, static_cast<f32>(rise), 0}, 1, 15, 1);
                }
                if (resting) {
                    next.y = resting->y + 1.2f;
                }
                reward.flying = false;
                reward.motion.velocity = Vec3{0};
            }
            reward.motion.position = next;
        }
        pickup.position = reward.motion.position;
        m_fixtures[reward.figure].placeAt(glm::translate(Mat4{1}, pickup.position));
    }
}

} // namespace gdl::game
