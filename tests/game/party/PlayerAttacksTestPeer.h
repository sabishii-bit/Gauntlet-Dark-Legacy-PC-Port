#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/MissileTarget.h"
#include "game/enemies/Enemies.h"
#include "game/mechanics/BlowRules.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {
/** The private parts of PlayerAttacks the tests drive directly: the target rosters, the
 * dispatchers that land one blow, and the waves and rings that carry them. */
struct PlayerAttacksTestPeer {
    static std::vector<MissileTarget> targets(const PlayerAttacks& attacks,
                                              const PlayerAttacks::Targets& targets) {
        return attacks.projectileTargets(targets);
    }
    static std::vector<MissileTarget> strikeTargets(const PlayerAttacks& attacks,
                                                    const PlayerAttacks::Targets& targets) {
        return attacks.strikeTargets(targets);
    }
    static std::vector<MissileTarget> acquisitionTargets(const PlayerAttacks& attacks,
                                                         const PlayerAttacks::Targets& targets) {
        return attacks.acquisitionTargets(targets);
    }
    static std::optional<MissileTarget> meleePlayer(const PlayerActor& actor,
                                                    const PlayerAttacks::Targets& targets,
                                                    f32 reach, const Vec3& facing) {
        return PlayerAttacks::meleePlayer(actor, targets, reach, facing);
    }
    static bool isCreature(s32 id) { return PlayerAttacks::isCreature(id); }
    static bool strikeSwitch(PlayerAttacks& attacks, s32 id, u32 flags) {
        return attacks.strikeSwitch(id, flags);
    }
    static bool immuneToMagic(const PlayerAttacks& attacks, s32 id,
                              const PlayerAttacks::Targets& targets) {
        return attacks.immuneToMagic(id, targets);
    }
    /** Whether a swing reckons `body` low enough for its low blow, by its family and height. */
    static bool meleeLow(const PlayerActor& actor, const MissileTarget& body) {
        return PlayerAttacks::meleeCandidate(actor, body).low;
    }
    /** The thrown weapons' flight over `seconds`, against the whole roster a flight meets. */
    static void flights(PlayerAttacks& attacks, f32 seconds, std::span<PlayerRuntime> players,
                        const PlayerAttacks::Targets& targets) {
        attacks.stepProjectileFlights(seconds, players, targets);
    }
    static void projectile(PlayerAttacks& attacks, const MissileImpact& impact,
                           std::span<PlayerRuntime> players,
                           const PlayerAttacks::Targets& targets) {
        attacks.dispatchProjectileImpact(impact, players, targets,
                                         [](const MissileImpact&, f32) {});
    }
    static void area(PlayerAttacks& attacks, const MissileTarget& target,
                     std::span<PlayerRuntime> players, const PlayerAttacks::Targets& targets,
                     BlowSource source) {
        strike(attacks, target, 35, EnemyHit::kKnockDown, players, targets, source);
    }
    /** An area blow of the first player's, as a turbo strike, a breath or a shield lands one. */
    static void strike(PlayerAttacks& attacks, const MissileTarget& target, f32 damage, u32 flags,
                       std::span<PlayerRuntime> players, const PlayerAttacks::Targets& targets,
                       BlowSource source) {
        attacks.strikeTarget(target, damage, flags, players.front().actor, players, targets,
                             source);
    }
    static void potion(PlayerAttacks& attacks) {
        MissileImpact impact;
        impact.owner = 3;
        impact.potion = 1;
        impact.potency = 20;
        impact.damage = 25;
        attacks.beginPotion(impact);
    }
    /** A wave of magic going off as `impact` says. */
    static void burst(PlayerAttacks& attacks, const MissileImpact& impact) {
        attacks.beginPotion(impact);
    }
    static void potions(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                        const PlayerAttacks::Targets& targets) {
        attacks.updatePotions(1.0f / 60, players, targets);
    }
    static void shield(PlayerAttacks& attacks) { ring(attacks, EnemyHit::kMagic, 10, 30); }
    /** A potion's ring about the first player, for a second. */
    static void ring(PlayerAttacks& attacks, u32 flags, f32 radius, f32 damage) {
        PlayerAttacks::PotionShield shield;
        shield.actor = 0;
        shield.flags = flags;
        shield.radius = radius;
        shield.damage = damage;
        shield.secondsLeft = 1;
        attacks.m_sim.shields.push_back(shield);
    }
};
} // namespace gdl::game
