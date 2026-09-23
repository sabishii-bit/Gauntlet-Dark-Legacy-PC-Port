#include "game/players/Relics.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

bool Relics::addRune(int rune) {
    if (!inRange(rune, kRuneCount) || hasRune(rune)) {
        return false;
    }
    runes |= bit(rune);
    return true;
}

int Relics::runeCount() const {
    return std::popcount(runes);
}

bool Relics::addLegend(int realm) {
    if (!inRange(realm, kRealmCount)) {
        return false;
    }
    legends |= bit(realm);
    return true;
}

bool Relics::spendLegend(int realm) {
    if (!hasLegend(realm)) {
        return false;
    }
    legends &= static_cast<std::uint16_t>(~bit(realm));
    return true;
}

bool Relics::addShard(int order) {
    if (!inRange(order, kRealmCount) || hasShard(order)) {
        return false;
    }
    shards |= bit(order);
    return true;
}

int Relics::addGargoylePiece(int kind) {
    if (!inRange(kind, static_cast<int>(kGargoyleKinds))) {
        return -1;
    }
    int& count = gargoylePieces[static_cast<std::size_t>(kind)];
    count = std::min(count + 1, kGargoyleNeeded[static_cast<std::size_t>(kind)]);
    return count;
}

} // namespace gdl::game
