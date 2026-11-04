#pragma once

#include <vector>

#include "engine/core/Types.h"

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"

namespace gdl::game {

/** Resource binding for native opponent simulation, not replica draw models.
 * IDs describe original enemy stock kind/slot pairs, never live shooter slots.
 * Used only by diagnostic captures; no resources are sent over the network. */
class OpponentResources {
public:
    static constexpr s32 kMissileSlots = 3;
    /** Zero is reserved for a genuinely model-less native missile; invalid kind
     * or slot also returns zero and must not be registered as a resource. */
    static u32 missileId(s32 kind, s32 slot);
    /** Inspect every native kind, including Death and the art-less IT, without
     * loading archives or creating textures. Missing/unbound models have no
     * entry. The caller must not clear/rebind enemies while these borrowed
     * original stock pointers or restored missiles are in use. */
    static std::vector<EnemyMissiles::Resource> missiles(const Enemies& enemies);
};

} // namespace gdl::game
