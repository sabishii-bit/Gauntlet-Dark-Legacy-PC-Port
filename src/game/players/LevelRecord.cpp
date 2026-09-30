#include "game/players/LevelRecord.h"

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

bool realmInRange(s32 realm) {
    return realm >= 0 && static_cast<usize>(realm) < LevelRecord::kRealmCount;
}

u8 levelBit(s32 level) {
    return static_cast<u8>(1U << static_cast<u32>(level));
}

} // namespace

bool LevelRecord::hasBeaten(s32 realm, s32 level) const {
    if (!realmInRange(realm) || level < 0 || level >= kLevelsPerRealm) {
        return false;
    }
    return (beaten[static_cast<usize>(realm)] & levelBit(level)) != 0;
}

void LevelRecord::pass(std::array<u16, kPasses>& passes, s32 bit) {
    if (bit < 0 || bit > kMostRealm) {
        return;
    }
    const auto mask = static_cast<u16>(1U << static_cast<u32>(bit));
    if ((passes[0] & mask) != 0) {
        passes[1] = static_cast<u16>(passes[1] | mask);
    } else {
        passes[0] = static_cast<u16>(passes[0] | mask);
    }
}

void LevelRecord::recordBeaten(s32 realm, s32 level, s32 rune, s32 legend) {
    if (rune > 0 && rune <= kRuneCount) {
        pass(runeLevels, rune - 1);
    }
    if (legend > 0) {
        pass(legendLevels, legend);
    }
    if (realmInRange(realm) && level >= 0 && level < kLevelsPerRealm) {
        beaten[static_cast<usize>(realm)] =
            static_cast<u8>(beaten[static_cast<usize>(realm)] | levelBit(level));
    }
}

void LevelRecord::recordBossDeath(s32 realm) {
    pass(bossDeaths, realm);
}

} // namespace gdl::game
