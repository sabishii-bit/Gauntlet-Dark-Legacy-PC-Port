#pragma once

#include <string_view>

#include "game/effects/EffectTrees.h"
#include "game/enemies/EnemyMissiles.h"

namespace gdl::game {
/** Native enemy-missile landing presentation. Resolution selects only the
 * already-bound shared weapon archive; it does not load art or start effects. */
struct EnemyMissileEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;
    std::string_view sound;
    f32 level = 0; ///< native 0..255 volume before nearest-player attenuation

    static EnemyMissileEmission resolve(const EnemyMissileHit& /*hit*/, ItemArchive& weapons);
};
} // namespace gdl::game
