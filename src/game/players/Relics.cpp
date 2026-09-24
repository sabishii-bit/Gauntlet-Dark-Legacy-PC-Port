#include "game/players/Relics.h"

#include <algorithm>
#include <bit>

#include "engine/core/Types.h"

namespace gdl::game {

bool Relics::addRune(s32 rune) {
    if (!inRange(rune, kRuneCount) || hasRune(rune)) {
        return false;
    }
    runes |= bit(rune);
    pendingRunes |= bit(rune);
    return true;
}

s32 Relics::runeCount() const {
    return std::popcount(runes);
}

bool Relics::addLegend(s32 realm) {
    if (!inRange(realm, kRealmCount)) {
        return false;
    }
    legends |= bit(realm);
    return true;
}

bool Relics::spendLegend(s32 realm) {
    if (!hasLegend(realm)) {
        return false;
    }
    legends &= static_cast<u16>(~bit(realm));
    return true;
}

bool Relics::addShard(s32 order) {
    if (!inRange(order, kRealmCount) || hasShard(order)) {
        return false;
    }
    shards |= bit(order);
    if (order >= 1 && order <= 8) {
        pendingShards |= bit(order);
    }
    return true;
}

s32 Relics::addGargoylePiece(s32 kind) {
    if (!inRange(kind, static_cast<s32>(kGargoyleKinds))) {
        return -1;
    }
    s32& count = gargoylePieces[static_cast<usize>(kind)];
    count = std::min(count + 1, kGargoyleNeeded[static_cast<usize>(kind)]);
    return count;
}

} // namespace gdl::game
