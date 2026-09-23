#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "engine/assets/WorldData.h"

namespace gdl::game {

/** What is known of a kind of enemy before any is seen: its size, pace, strength and worth. */
struct EnemyKind {
    std::string_view name;   ///< as the level's generators name it ("GRU")
    std::string_view prefix; ///< what its trees and generator bodies are called by ("GRU")
    float height;            ///< of its body; half of it is the reach up from its feet
    float radius;            ///< of its body
    float attentionHeight;   ///< where eyes and missiles are aimed
    float collisionHeight;   ///< where its body is struck
    float pace;              ///< how far it goes in a tick at a level's speed of one
    float damage;            ///< what its blows deal at full strength
    float armor;
    float health;                ///< at full strength, before the level's and its tier's scales
    float generatorArmor;        ///< of the generator that breeds it
    std::int32_t experienceHit;  ///< won by a blow that hurts it
    std::int32_t experienceKill; ///< and by the one that kills it
    std::int32_t algorithm;      ///< the way it goes about when a generator gives it none
    float turnRate;              ///< radians a tick

    /** The tier's share of the full health: a third of it a tier, up to three tiers. */
    float healthAtTier(std::int32_t tier) const;
};

/** The kinds in the original's order: the swarm first, then the great ones. */
inline constexpr std::int32_t kEnemyKindCount = 34;
inline constexpr std::int32_t kSwarmKindCount = 28; ///< the kinds that come in tiers
inline constexpr std::int32_t kGruntKind = 4;
inline constexpr std::int32_t kRatKind = 3;
inline constexpr std::int32_t kDeathKind = 30;
inline constexpr std::int32_t kGolemEnemyKind = 29;
inline constexpr std::int32_t kGargoyleEnemyKind = 32;
inline constexpr std::int32_t kGeneralEnemyKind = 33;

const EnemyKind& enemyKind(std::int32_t kind);

/** The kind a generator or placement names, in any case; nullopt for a name not known. */
std::optional<std::int32_t> enemyKindOf(std::string_view name);

/** The classes a realm's roster sorts its kinds into. */
inline constexpr std::int32_t kSmallClass = 1;
inline constexpr std::int32_t kMediumClass = 2;
inline constexpr std::int32_t kLargeClass = 3;
inline constexpr std::int32_t kMediumOtherClass =
    4; ///< the medium's second row: the strong variants

/** The kind a level really breeds for one a generator or placement names: the names in a
 * level are stand-ins for a class (a rat for the small, a grunt or a knight for the
 * medium, or the medium's second row at strength four and over, else the large), and the
 * level's roster says which kind fills each; a name of no class, or a class the roster
 * lacks, stands for itself. */
std::int32_t levelKindOf(std::span<const LevelEnemy> roster, std::int32_t named,
                         std::int32_t strength);

} // namespace gdl::game
