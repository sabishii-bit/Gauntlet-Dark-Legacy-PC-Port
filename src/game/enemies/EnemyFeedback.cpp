#include "game/enemies/EnemyFeedback.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

#include "game/combat/DamageTypes.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/BossKinds.h"

namespace gdl::game {
namespace {
constexpr f32 kLowEnemyHeight = 4.0f;
/** The frames every death skin runs, the knight's and the tree folk's DEATHALT as much as the
 * element skins (damage_enemy's SetSkinFX calls, 0x8004EE88, 0x8004EEB8 and 0x8004EEF8). */
constexpr s32 kDeathSkinFrames = 10;

/** How an unelemental hit and death look on a hide (fn_800945D0): the marks of a hit and of a
 * kill, whether the mark keeps its own size, and the death skin. An empty mark or skin is the
 * element's own. */
struct HideLook {
    std::string_view hitEffect;
    std::string_view killEffect;
    bool unscaledEffect;
    std::string_view deathSkin;
};

/** Flesh, plate (the knight), bark (the tree folk) and ooze (the acid blob), in Hide order. */
constexpr std::array<HideLook, 4> kHides{{
    {{}, {}, false, {}},
    {"HITCOL", "HITDIE", true, "DEATHALT"},
    {"TREEHIT", "TREEDIE", false, "DEATHALT"},
    {"TREEHIT", "TREEDIE", false, {}},
}};

/** The look of `kind`'s hide; flesh outside the kinds. */
const HideLook& hideOf(s32 kind) {
    const EnemyTraits* traits = enemyTraits(kind);
    usize row = 0;
    switch (traits != nullptr ? traits->hide : Hide::Flesh) {
    case Hide::Flesh: row = 0; break;
    case Hide::Plate: row = 1; break;
    case Hide::Bark: row = 2; break;
    case Hide::Ooze: row = 3; break;
    }
    return kHides[row];
}

std::string bankName(std::string name, s32 bossType) {
    // Some boss banks replace byte 14 with a variant letter; bank keys hold 15 characters.
    const BossStage* stage = bossStageOf(bossType);
    const char variant = stage != nullptr ? stage->swarmBankVariant : '\0';
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
    const HideLook& hide = hideOf(kind);
    if (element == 0 && !hide.hitEffect.empty()) {
        return killed ? hide.killEffect : hide.hitEffect;
    }
    return damage::hitEffect(element, killed);
}

f32 EnemyFeedback::effectScale() const {
    return hideOf(kind).unscaledEffect ? 1.0f : halfHeight * 0.5f;
}

std::string_view EnemyFeedback::deathSkin() const {
    constexpr std::array<std::string_view, 5> kSkins{"DEATHBLOOD", "DEATHFIRE", "DEATHELEC",
                                                     "DEATHLIGHT", "DEATHACID"};
    const u32 element = flags & damage::kElementMask;
    const HideLook& hide = hideOf(kind);
    if (element == 0 && !hide.deathSkin.empty()) {
        return hide.deathSkin;
    }
    return halfHeight > 2 && element < kSkins.size() ? kSkins[element] : std::string_view{};
}

s32 EnemyFeedback::deathSkinFrames() const {
    return deathSkin().empty() ? 0 : kDeathSkinFrames;
}
} // namespace gdl::game
