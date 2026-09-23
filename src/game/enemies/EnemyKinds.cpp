#include "game/enemies/EnemyKinds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <numbers>

namespace gdl::game {

namespace {

// A sixty-fourth of a half turn a tick, the one turning pace every kind shares.
constexpr float kTurn = std::numbers::pi_v<float> / 64.0f;

// name, prefix, height, radius, attention, collision, pace, damage, armor, health, generator
// armor, experience for a hit and for a kill, algorithm, turn rate. The twenty-ninth row is
// the original's sentinel between the swarm and the great ones.
constexpr std::array<EnemyKind, kEnemyKindCount> kKinds{{
    {"SCO", "SCO", 3.0f, 1.5f, 2.0f, 1.5f, 0.1f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 2, kTurn},
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
    {"ACI", "ACI", 3.0f, 0.75f, 0.5f, 1.5f, 0.02f, 12.0f, 0.0f, 21.0f, 3.0f, 1, 2, 7, kTurn},
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

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

} // namespace

float EnemyKind::healthAtTier(int tier) const {
    return health * 0.333f * static_cast<float>(std::clamp(tier, 1, 3));
}

const EnemyKind& enemyKind(int kind) {
    return kKinds[static_cast<std::size_t>(std::clamp(kind, 0, kEnemyKindCount - 1))];
}

int levelKindOf(std::span<const LevelEnemy> roster, int named, int strength) {
    const auto ofClass = [&roster](int subtype) -> std::optional<int> {
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
    if (named == kGruntKind || named == kGruntKind + 1) {
        if (strength >= 4) {
            if (const auto other = ofClass(kMediumOtherClass); other.has_value()) {
                return *other;
            }
        }
        return ofClass(kMediumClass).value_or(ofClass(kLargeClass).value_or(named));
    }
    return named;
}

std::optional<int> enemyKindOf(std::string_view name) {
    for (int i = 0; i < kEnemyKindCount; ++i) {
        const EnemyKind& kind = kKinds[static_cast<std::size_t>(i)];
        if (kind.height > 0.0f && (sameName(kind.name, name) || sameName(kind.prefix, name))) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::game
