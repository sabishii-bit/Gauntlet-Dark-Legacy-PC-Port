#include "game/enemies/EnemyFeedback.h"

#include <array>
#include <format>

#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {
bool hasTierSounds(s32 kind) {
    switch (kind) {
    case 1:
    case 2:
    case 4:
    case 5:
    case 7:
    case 8:
    case 10:
    case 11:
    case 13:
    case 14:
    case 16:
    case 17:
    case 19:
    case 20:
    case 23:
    case 24:
    case 25:
    case 26: return true;
    default: return false;
    }
}
} // namespace

std::string EnemyFeedback::sound(bool bossLevel) const {
    if (kind < 0 || kind >= kSwarmKindCount) {
        return {};
    }
    const bool tiered = hasTierSounds(kind);
    const bool large = tier > 1 || bossLevel;
    const std::string_view suffix = close ? "CLOSE" : "FAR";
    std::string stem{enemyKind(kind).prefix};
    if (tiered) {
        stem += large ? '2' : '1';
    } else if (kind == 27) {
        stem += '1';
    }
    if (killed) {
        return std::format("S_{}DIE{}", stem, suffix);
    }
    if (tiered && large) {
        return std::format("S_{}HIT{}{}", stem, hitCount < 2 ? 1 : 2, suffix);
    }
    return std::format("S_{}HIT{}", stem, suffix);
}

std::string_view EnemyFeedback::effect() const {
    constexpr u32 kNoImpact = 0x1000000;
    if ((flags & kNoImpact) != 0) {
        return {};
    }
    constexpr std::array<std::string_view, 5> kHits{"BLOODFX1", "FIREHIT", "HITCOL", "HITCOL",
                                                    "HITCOL"};
    constexpr std::array<std::string_view, 5> kDeaths{"BLOODFX2", "FIREDIE", "ELECDIE", "LIGHTDIE",
                                                      "ACIDDIE"};
    const u32 element = flags & 0xF;
    if (element >= kHits.size()) {
        return {};
    }
    if (element == 0 && kind == 5) {
        return killed ? "HITDIE" : "HITCOL";
    }
    if (element == 0 && (kind == 11 || kind == 21)) {
        return killed ? "TREEDIE" : "TREEHIT";
    }
    return killed ? kDeaths[element] : kHits[element];
}

f32 EnemyFeedback::effectScale() const {
    return kind == 5 ? 1.0f : halfHeight * 0.5f;
}

std::string_view EnemyFeedback::deathSkin() const {
    constexpr std::array<std::string_view, 5> kSkins{"DEATHBLOOD", "DEATHFIRE", "DEATHELEC",
                                                     "DEATHLIGHT", "DEATHACID"};
    const u32 element = flags & 0xF;
    if (element == 0 && (kind == 5 || kind == 11)) {
        return "DEATHALT";
    }
    return halfHeight > 2 && element < kSkins.size() ? kSkins[element] : std::string_view{};
}
} // namespace gdl::game
