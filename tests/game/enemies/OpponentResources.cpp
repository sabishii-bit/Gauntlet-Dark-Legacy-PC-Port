#include "game/enemies/OpponentResources.h"

#include <cstddef>

namespace gdl::game {

u32 OpponentResources::missileId(s32 kind, s32 slot) {
    if (kind < 0 || kind >= kEnemyKindCount || slot < 0 || slot >= kMissileSlots) {
        return 0;
    }
    return 1 + static_cast<u32>(kind * kMissileSlots + slot);
}

std::vector<EnemyMissiles::Resource> OpponentResources::missiles(const Enemies& enemies) {
    static_assert(static_cast<usize>(kEnemyKindCount) * static_cast<usize>(kMissileSlots) <=
                  EnemyMissiles::Checkpoint::kMaxResources);
    std::vector<EnemyMissiles::Resource> result;
    result.reserve(static_cast<usize>(kEnemyKindCount) * static_cast<usize>(kMissileSlots));
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        for (s32 slot = 0; slot < kMissileSlots; ++slot) {
            if (const auto* model = enemies.projectileModel(kind, slot)) {
                result.push_back({missileId(kind, slot), model});
            }
        }
    }
    return result;
}

} // namespace gdl::game
