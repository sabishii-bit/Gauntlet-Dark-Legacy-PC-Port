#include "game/screens/TowerCompletion.h"

#include "game/world/TowerAccess.h"

namespace gdl::game {

u16 TowerCompletion::pending(std::span<const Relics> party) {
    u16 runes = 0;
    u16 shards = 0;
    u16 installedRunes = 0;
    u16 installedShards = 0;
    u16 pendingShards = 0;
    u16 result = 0;
    for (const auto& relics : party) {
        runes |= relics.runes;
        shards |= relics.shards;
        installedRunes |= static_cast<u16>(relics.runes & ~relics.pendingRunes);
        installedShards |= static_cast<u16>(relics.shards & ~relics.pendingShards);
        pendingShards |= relics.pendingShards;
        result |= relics.pendingCeremonies;
    }
    const auto holds = [](u16 held, u16 wanted) { return (held & wanted) == wanted; };
    const bool window = holds(shards, TowerAccess::kTempleShards);
    const bool twelve = holds(runes, TowerAccess::kUnderworldRunes);
    const bool skorne = holds(shards, TowerAccess::kUnderworldShards);
    const bool thirteen = holds(runes, TowerAccess::kGarmRunes);
    if ((shards & ~installedShards & TowerAccess::kTempleShards) != 0) {
        result |= bit(window ? Kind::Window : Kind::MoreShards);
    }
    constexpr u16 kTempleShard = 1U << 9;
    if (twelve && ((runes & ~installedRunes & TowerAccess::kUnderworldRunes) != 0 ||
                   (pendingShards & ~installedShards & kTempleShard) != 0)) {
        result |= bit(skorne ? Kind::Underworld : Kind::TwelveWaiting);
    }
    constexpr u16 kLastRune = 1U << 12;
    if (thirteen && (runes & ~installedRunes & kLastRune) != 0) {
        result |= bit(Kind::Garm);
    }
    // Interrupted ceremonies may return with a different party. Never reveal an
    // unearned route, or repeat an obsolete "still needed" speech.
    if (window) {
        result &= static_cast<u16>(~bit(Kind::MoreShards));
    } else {
        result &= static_cast<u16>(~bit(Kind::Window));
    }
    if (!twelve) {
        result &= static_cast<u16>(~(bit(Kind::TwelveWaiting) | bit(Kind::Underworld)));
    } else if (skorne) {
        if ((result & bit(Kind::TwelveWaiting)) != 0) {
            result |= bit(Kind::Underworld);
        }
        result &= static_cast<u16>(~bit(Kind::TwelveWaiting));
    } else {
        result &= static_cast<u16>(~bit(Kind::Underworld));
    }
    if (!thirteen) {
        result &= static_cast<u16>(~bit(Kind::Garm));
    }
    return result & kKnown;
}

std::string_view TowerCompletion::message(Kind kind) {
    switch (kind) {
    case Kind::MoreShards: return "MORESHARDS";
    case Kind::Window: return "ALLSHARDS";
    case Kind::TwelveWaiting: return "ALL12RUNESNO";
    case Kind::Underworld: return "ALL12RUNESYES";
    case Kind::Garm: return "RUNE13YES";
    }
    return {};
}
std::string_view TowerCompletion::voice(Kind kind) {
    switch (kind) {
    case Kind::MoreShards: return "S_CONTINUEVOX";
    case Kind::Window: return "S_4KEYVOX";
    case Kind::TwelveWaiting: return "S_12RUNENO";
    case Kind::Underworld: return "S_12RUNEYES";
    case Kind::Garm: return "S_RUNE13YES";
    }
    return {};
}
u32 TowerCompletion::camera(Kind kind) {
    switch (kind) {
    case Kind::Window: return 204;
    case Kind::Underworld: return 201;
    case Kind::Garm: return 205;
    default: return 0;
    }
}
std::string_view TowerCompletion::portal(Kind kind) {
    switch (kind) {
    case Kind::Window: return "e1";
    case Kind::Underworld: return "f1";
    case Kind::Garm: return "h4";
    default: return {};
    }
}

} // namespace gdl::game
