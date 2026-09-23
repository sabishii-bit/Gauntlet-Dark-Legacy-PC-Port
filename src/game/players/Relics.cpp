#include "game/players/Relics.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

bool Relics::addRune(std::int32_t rune) {
    if (!inRange(rune, kRuneCount) || hasRune(rune)) {
        return false;
    }
    runes |= bit(rune);
    return true;
}

std::int32_t Relics::runeCount() const {
    return std::popcount(runes);
}

bool Relics::addLegend(std::int32_t realm) {
    if (!inRange(realm, kRealmCount)) {
        return false;
    }
    legends |= bit(realm);
    return true;
}

bool Relics::spendLegend(std::int32_t realm) {
    if (!hasLegend(realm)) {
        return false;
    }
    legends &= static_cast<std::uint16_t>(~bit(realm));
    return true;
}

bool Relics::addShard(std::int32_t order) {
    if (!inRange(order, kRealmCount) || hasShard(order)) {
        return false;
    }
    shards |= bit(order);
    return true;
}

std::int32_t Relics::addGargoylePiece(std::int32_t kind) {
    if (!inRange(kind, static_cast<std::int32_t>(kGargoyleKinds))) {
        return -1;
    }
    std::int32_t& count = gargoylePieces[static_cast<std::size_t>(kind)];
    count = std::min(count + 1, kGargoyleNeeded[static_cast<std::size_t>(kind)]);
    return count;
}

} // namespace gdl::game
