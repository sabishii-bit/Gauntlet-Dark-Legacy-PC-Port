#include "game/enemies/EnemyKinds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <numbers>

#include "engine/core/Types.h"

#include "game/enemies/EnemyMind.h"

namespace gdl::game {

namespace {

// A sixty-fourth of a half turn a tick, the one turning pace every kind shares.
constexpr f32 kTurn = std::numbers::pi_v<f32> / 64.0f;

// name, prefix, height, radius, attention, collision, pace, damage, armor, health, generator
// armor, experience for a hit and for a kill, algorithm, turn rate, then the idle action and
// rootedness where they are not the usual. The twenty-ninth row is the original's sentinel
// between the swarm and the great ones.
constexpr std::array<EnemyKind, kEnemyKindCount> kKinds{{
    {"SCO", "SCO", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 2, kTurn,
     EnemyAction::Walk},
    {"TRO", "TRO", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"DEM", "DEM", 6.0f, 1.8f, 3.8f, 3.0f, 0.12f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"RAT", "RAT", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 2, kTurn},
    {"GRU", "GRU", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"KNI", "KNI", 6.0f, 1.8f, 3.8f, 3.0f, 0.1f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"SNA", "SNA", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
    {"SOR", "SOR", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"MUM", "MUM", 6.0f, 1.8f, 3.8f, 3.0f, 0.1f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"SPI", "SPI", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
    {"LIZ", "LIZ", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"TRE", "TRE", 6.0f, 1.8f, 3.8f, 3.0f, 0.1f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"MAG", "MAG", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 2, kTurn},
    {"ZOM", "ZOM", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"PLA", "PLA", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"WOL", "WOL", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
    {"ICE", "ICE", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"WRM", "WRM", 6.0f, 1.8f, 3.8f, 3.0f, 0.1f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"DOG", "DOG", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
    {"SKE", "SKE", 6.0f, 1.5f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"GHO", "GHO", 6.0f, 1.8f, 3.8f, 3.0f, 0.1f, 18.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"ACI", "ACI", 3.0f, 0.75f, 0.5f, 1.5f, 0.02f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn,
     EnemyAction::Ready, true},
    {"HAN", "HAN", 3.0f, 0.75f, 0.5f, 1.5f, 0.05f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
    {"IMP", "IMP", 6.0f, 2.0f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 30.0f, 3.0f, 2, 4, 7, kTurn},
    {"WAR", "WAR", 6.0f, 2.0f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"SKY", "SKY", 6.0f, 2.0f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"WIND", "WIND", 6.0f, 2.0f, 3.8f, 3.0f, 0.1f, 15.0f, 0.0f, 46.0f, 3.0f, 3, 6, 7, kTurn},
    {"GRM", "GRM", 10.0f, 4.0f, 3.8f, 4.0f, 0.1f, 20.0f, 0.0f, 100.0f, 3.0f, 15, 20, 7, kTurn},
    {"NONE", "NONE", -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0.0f},
    {"GOLEM", "GOLEM", 12.0f, 3.0f, 5.0f, 4.0f, 0.09f, 20.0f, 0.0f, 200.0f, 3.0f, 30, 40, 19,
     kTurn},
    {"DEATH", "DEATH", 6.0f, 1.5f, 3.0f, 3.0f, 0.125f, 1.0f, 1.0f, 100.0f, 0.0f, 1, 1, 3, kTurn},
    {"IT", "IT", 5.0f, 1.5f, 3.0f, 3.0f, 0.1f, 0.0f, 0.0f, 9999.0f, 0.0f, 2, 4, 27, kTurn},
    {"GAR", "GAR", 10.0f, 6.0f, 5.0f, 3.0f, 0.1f, 30.0f, 0.0f, 500.0f, 0.0f, 300, 300, 7, kTurn},
    {"GENERAL", "GEN", 6.0f, 2.0f, 4.0f, 3.0f, 0.09f, 20.0f, 0.0f, 200.0f, 3.0f, 30, 40, 7, kTurn},
}};

/** One row a kind, in code order. The ways are fn_8004F87C's (init_enemy_vars): the small kinds
 * only prowl, a medium kind placed with no way takes its strength's, and Garm's brood and IT
 * keep their own whatever they are placed with. The humanoids are born only ahead of their
 * generator and the dog by its front or back (generate_enemy, GC 0x8004f4b4); the worm comes
 * up on its generator's own spot. A level's placements of Death and IT stand in the swarm's
 * pool, outside its tiers, and the great ones by their own statue and carried-item loaders. */
constexpr std::array<EnemyTraits, kEnemyKindCount> kTraits{{
    {.kind = enemy_type::kScorpion, .way = WayRule::Small, .traits = enemy_trait::kNoShadow},
    {.kind = enemy_type::kTroll,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants,
     .traits = enemy_trait::kWalksIn},
    {.kind = enemy_type::kDemon, .way = WayRule::Caster, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kRat, .way = WayRule::Small},
    {.kind = enemy_type::kGrunt,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants,
     .traits = enemy_trait::kWalksIn},
    {.kind = enemy_type::kKnight,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .hide = Hide::Plate,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kSnake, .way = WayRule::Small},
    {.kind = enemy_type::kSorcerer,
     .way = WayRule::Caster,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants,
     .traits = enemy_trait::kWalksIn},
    {.kind = enemy_type::kMummy,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kSpider, .way = WayRule::Small},
    {.kind = enemy_type::kLizardman,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants,
     .traits = enemy_trait::kWalksIn},
    {.kind = enemy_type::kTreefolk,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .hide = Hide::Bark,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kMaggot, .way = WayRule::Small},
    {.kind = enemy_type::kZombie, .way = WayRule::Medium, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kPlague,
     .way = WayRule::Caster,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kWolf, .way = WayRule::Small, .birthExits = kAheadOctants},
    {.kind = enemy_type::kIce, .way = WayRule::Medium, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kWorm,
     .way = WayRule::Caster,
     .stem = SoundStem::Tiered,
     .traits = enemy_trait::kBurrows},
    {.kind = enemy_type::kDog, .way = WayRule::Small, .birthDirections = 2},
    {.kind = enemy_type::kSkeleton,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kGhost, .way = WayRule::Medium, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kAcid,
     .way = WayRule::Small,
     .hide = Hide::Ooze,
     .traits = enemy_trait::kNoShadow},
    {.kind = enemy_type::kHand, .way = WayRule::Small},
    {.kind = enemy_type::kImp, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kWarlock,
     .way = WayRule::Caster,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants,
     .traits = enemy_trait::kVeils},
    {.kind = enemy_type::kSky,
     .way = WayRule::Medium,
     .stem = SoundStem::Tiered,
     .birthExits = kAheadOctants},
    {.kind = enemy_type::kWhirlwind, .stem = SoundStem::Tiered},
    {.kind = enemy_type::kGarm2,
     .fixedWay = kLungeWay,
     .traits = enemy_trait::kDeathShot | enemy_trait::kHazardProof},
    {.kind = kSwarmKindCount, .home = EnemyHome::None},
    {.kind = enemy_type::kGolem, .home = EnemyHome::Critters},
    {.kind = enemy_type::kDeath, .traits = enemy_trait::kNoShadow, .home = EnemyHome::PlacedOnly},
    {.kind = enemy_type::kIt,
     .fixedWay = kLurkWay,
     .traits = enemy_trait::kNoShadow | enemy_trait::kIntangible,
     .home = EnemyHome::PlacedOnly},
    {.kind = enemy_type::kGargoyle, .way = WayRule::Medium, .home = EnemyHome::Critters},
    {.kind = enemy_type::kGeneral, .way = WayRule::Medium, .home = EnemyHome::Critters},
}};
static_assert(
    [] {
        for (usize i = 0; i < kTraits.size(); ++i) {
            if (kTraits[i].kind != static_cast<s32>(i)) {
                return false;
            }
        }
        return true;
    }(),
    "each kind's traits sit at its code's row");

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

} // namespace

f32 EnemyKind::healthAtTier(s32 tier) const {
    return health * 0.333f * static_cast<f32>(std::clamp(tier, 1, 3));
}

const EnemyKind& enemyKind(s32 kind) {
    return kKinds[static_cast<usize>(std::clamp(kind, 0, kEnemyKindCount - 1))];
}

bool smallKind(s32 kind) {
    const EnemyTraits* traits = enemyTraits(kind);
    return traits != nullptr && traits->way == WayRule::Small;
}

bool tieredSounds(s32 kind) {
    const EnemyTraits* traits = enemyTraits(kind);
    return traits != nullptr && traits->stem == SoundStem::Tiered;
}

const EnemyTraits* enemyTraits(s32 kind) {
    if (kind < 0 || kind >= kEnemyKindCount) {
        return nullptr;
    }
    return &kTraits[static_cast<usize>(kind)];
}

bool hasEnemyTrait(s32 kind, u16 trait) {
    const EnemyTraits* traits = enemyTraits(kind);
    return traits != nullptr && (traits->traits & trait) != 0;
}

bool intangible(s32 kind) {
    return hasEnemyTrait(kind, enemy_trait::kIntangible);
}

s32 levelKindOf(std::span<const LevelEnemy> roster, s32 named, s32 strength) {
    const auto ofClass = [&roster](s32 subtype) -> std::optional<s32> {
        for (const LevelEnemy& enemy : roster) {
            if (enemy.subtype == subtype && enemy.kind >= 0) {
                return enemy.kind;
            }
        }
        return std::nullopt;
    };
    if (named == kRatKind) {
        return ofClass(kSmallClass).value_or(named);
    }
    if (named == enemy_type::kGrunt || named == enemy_type::kKnight) {
        if (strength >= 4) {
            if (const auto other = ofClass(kMediumOtherClass); other.has_value()) {
                return *other;
            }
        }
        return ofClass(kMediumClass).value_or(ofClass(kLargeClass).value_or(named));
    }
    return named;
}

std::optional<s32> enemyKindOf(std::string_view name) {
    for (s32 i = 0; i < kEnemyKindCount; ++i) {
        const EnemyKind& kind = kKinds[static_cast<usize>(i)];
        if (kind.height > 0.0f && (sameName(kind.name, name) || sameName(kind.prefix, name))) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::game
