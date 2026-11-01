#include "game/enemies/EnemyFeedback.h"

#include <algorithm>
#include <array>
#include <format>

#include "game/combat/DamageTypes.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {
constexpr f32 kLowEnemyHeight = 4.0f;

std::string bankName(std::string name, s32 bossType) {
    // Some boss banks replace byte 14 with a variant letter; bank keys hold 15 characters.
    char variant = '\0';
    switch (bossType) {
    case 41: variant = 'B'; break;
    case 37: variant = 'D'; break;
    case 36: variant = 'C'; break;
    default: break;
    }
    if (variant != '\0') {
        name.resize(std::min(name.size(), usize{14}));
        name += variant;
    }
    name.resize(std::min(name.size(), usize{15}));
    return name;
}
} // namespace

std::optional<std::string>
EnemyFeedback::meleeSound(s32 kind, s32 tier, std::span<const LevelEnemy> roster, s32 bossType) {
    if (kind < 0 || kind >= kSwarmKindCount) {
        return std::nullopt;
    }
    if (kind == enemy_type::kGrunt || kind == enemy_type::kKnight ||
        kind == enemy_type::kLizardman) {
        return tier >= 2 ? "S_PLYRDMG4" : "S_PLYRDMG5";
    }
    if (enemyKind(kind).height > kLowEnemyHeight && kind != enemy_type::kDemon &&
        kind != enemy_type::kMummy && kind != enemy_type::kTreefolk) {
        return std::nullopt;
    }
    const auto row = std::ranges::find(roster, kind, &LevelEnemy::kind);
    if (row == roster.end() || row->stream.empty()) {
        return std::string{};
    }
    std::string stem = row->stream;
    if (tieredSounds(kind)) {
        stem += tier > 1 || row->subtype >= 10 || bossType >= 0 ? '2' : '1';
    }
    return bankName(std::format("S_{}{}", stem, kind == enemy_type::kTreefolk ? "STRIKE" : "BITE"),
                    bossType);
}

std::string EnemyFeedback::sound(std::span<const LevelEnemy> roster, s32 bossType) const {
    if (kind < 0 || kind >= kSwarmKindCount) {
        return {};
    }
    const auto row = std::ranges::find(roster, kind, &LevelEnemy::kind);
    if (row == roster.end() || row->stream.empty()) {
        return {};
    }
    const bool tiered = tieredSounds(kind);
    // Classes >= 10 bind both sizes to the large sound set. This includes
    // the Temple's four species, regardless of the current actor's tier.
    const bool large = tier > 1 || row->subtype >= 10 || bossType >= 0;
    const std::string_view suffix = close ? "CLOSE" : "FAR";
    std::string stem{row->stream};
    if (tiered) {
        stem += large ? '2' : '1';
    } else if (kind == enemy_type::kGarm2) {
        stem += '1';
    }
    std::string name;
    if (killed) {
        name = std::format("S_{}DIE{}", stem, suffix);
    } else if (tiered && large) {
        name = std::format("S_{}HIT{}{}", stem, hitCount < 2 ? 1 : 2, suffix);
    } else {
        name = std::format("S_{}HIT{}", stem, suffix);
    }
    return bankName(name, bossType);
}

std::string_view EnemyFeedback::effect() const {
    if (!damage::marks(flags)) {
        return {};
    }
    const u32 element = damage::element(flags);
    if (element == 0 && kind == enemy_type::kKnight) {
        return killed ? "HITDIE" : "HITCOL";
    }
    if (element == 0 && (kind == enemy_type::kTreefolk || kind == enemy_type::kAcid)) {
        return killed ? "TREEDIE" : "TREEHIT";
    }
    return damage::hitEffect(element, killed);
}

f32 EnemyFeedback::effectScale() const {
    return kind == enemy_type::kKnight ? 1.0f : halfHeight * 0.5f;
}

std::string_view EnemyFeedback::deathSkin() const {
    constexpr std::array<std::string_view, 5> kSkins{"DEATHBLOOD", "DEATHFIRE", "DEATHELEC",
                                                     "DEATHLIGHT", "DEATHACID"};
    const u32 element = flags & damage::kElementMask;
    if (element == 0 && (kind == enemy_type::kKnight || kind == enemy_type::kTreefolk)) {
        return "DEATHALT";
    }
    return halfHeight > 2 && element < kSkins.size() ? kSkins[element] : std::string_view{};
}

s32 EnemyFeedback::deathSkinFrames() const {
    if (deathSkin().empty()) {
        return 0;
    }
    return kind == enemy_type::kKnight && (flags & damage::kElementMask) == 0 ? 15 : 10;
}
} // namespace gdl::game
