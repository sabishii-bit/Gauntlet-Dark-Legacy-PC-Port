#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"

namespace gdl::game {

/** What is known of a kind of enemy before any is seen: its size, pace, strength and worth. */
struct EnemyKind {
    std::string_view name;   ///< as the level's generators name it ("GRU")
    std::string_view prefix; ///< what its trees and generator bodies are called by ("GRU")
    f32 height;              ///< of its body; half of it is the reach up from its feet
    f32 radius;              ///< of its body
    f32 attentionHeight;     ///< where eyes and missiles are aimed
    f32 collisionHeight;     ///< where its body is struck
    f32 pace;                ///< how far it goes in a tick at a level's speed of one
    f32 damage;              ///< what its blows deal at full strength
    f32 armor;
    f32 health;         ///< at full strength, before the level's and its tier's scales
    f32 generatorArmor; ///< of the generator that breeds it
    s32 experienceHit;  ///< won by a blow that hurts it
    s32 experienceKill; ///< and by the one that kills it
    s32 algorithm;      ///< the way it goes about when a generator gives it none
    f32 turnRate;       ///< radians a tick

    /** The tier's share of the full health: a third of it a tier, up to three tiers. */
    f32 healthAtTier(s32 tier) const;
};

/** The kinds in the original's order: the swarm first, then the great ones. */
inline constexpr s32 kEnemyKindCount = 34;
inline constexpr s32 kSwarmKindCount = 28; ///< the kinds that come in tiers
inline constexpr s32 kGruntKind = 4;
inline constexpr s32 kRatKind = 3;
inline constexpr s32 kDeathKind = 30;
inline constexpr s32 kItKind = 31;
inline constexpr s32 kGolemEnemyKind = 29;
inline constexpr s32 kGargoyleEnemyKind = 32;
inline constexpr s32 kGeneralEnemyKind = 33;

const EnemyKind& enemyKind(s32 kind);

/** The kind a generator or placement names, in any case; nullopt for a name not known. */
std::optional<s32> enemyKindOf(std::string_view name);

/** The classes a realm's roster sorts its kinds into. */
inline constexpr s32 kSmallClass = 1;
inline constexpr s32 kMediumClass = 2;
inline constexpr s32 kLargeClass = 3;
inline constexpr s32 kMediumOtherClass = 4; ///< the medium's second row: the strong variants

/** The kind a level really breeds for one a generator or placement names: the names in a
 * level are stand-ins for a class (a rat for the small, a grunt or a knight for the
 * medium, or the medium's second row at strength four and over, else the large), and the
 * level's roster says which kind fills each; a name of no class, or a class the roster
 * lacks, stands for itself. */
s32 levelKindOf(std::span<const LevelEnemy> roster, s32 named, s32 strength);

} // namespace gdl::game
