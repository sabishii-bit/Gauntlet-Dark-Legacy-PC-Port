#pragma once

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
    float health;         ///< at full strength, before the level's and its tier's scales
    float generatorArmor; ///< of the generator that breeds it
    int experienceHit;    ///< won by a blow that hurts it
    int experienceKill;   ///< and by the one that kills it
    int algorithm;        ///< the way it goes about when a generator gives it none
    float turnRate;       ///< radians a tick

    /** The tier's share of the full health: a third of it a tier, up to three tiers. */
    float healthAtTier(int tier) const;
};

/** The kinds in the original's order: the swarm first, then the great ones. */
inline constexpr int kEnemyKindCount = 34;
inline constexpr int kSwarmKindCount = 28; ///< the kinds that come in tiers
inline constexpr int kGruntKind = 4;
inline constexpr int kRatKind = 3;
inline constexpr int kDeathKind = 30;
inline constexpr int kGolemEnemyKind = 29;
inline constexpr int kGargoyleEnemyKind = 32;
inline constexpr int kGeneralEnemyKind = 33;

const EnemyKind& enemyKind(int kind);

/** The kind a generator or placement names, in any case; nullopt for a name not known. */
std::optional<int> enemyKindOf(std::string_view name);

/** The classes a realm's roster sorts its kinds into. */
inline constexpr int kSmallClass = 1;
inline constexpr int kMediumClass = 2;
inline constexpr int kLargeClass = 3;
inline constexpr int kMediumOtherClass = 4; ///< the medium's second row: the strong variants

/** The kind a level really breeds for one a generator or placement names: the names in a
 * level are stand-ins for a class (a rat for the small, a grunt or a knight for the
 * medium, or the medium's second row at strength four and over, else the large), and the
 * level's roster says which kind fills each; a name of no class, or a class the roster
 * lacks, stands for itself. */
int levelKindOf(std::span<const LevelEnemy> roster, int named, int strength);

} // namespace gdl::game
