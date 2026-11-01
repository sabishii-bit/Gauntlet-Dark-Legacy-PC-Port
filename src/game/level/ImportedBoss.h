#pragma once

#include <string>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::game {

/** An explicitly selected GDL combatant in an imported arena. The donor names
 * select artwork/effects/audio only, never the imported campaign's identity. */
struct ImportedBoss {
    s32 kind = -1;
    Vec3 position{0}; ///< authored placement, before the native supporting-floor query
    f32 yaw = 0;
    std::string counterpartStage;
    std::string counterpartWorld;
    std::string soundBank;
    struct ArenaRock {
        Vec3 position{0};
        f32 yaw = 0;
        f32 radius = 2.3f;
        f32 height = 5;
        s32 tierHealth = 40;
    };
    std::vector<ArenaRock> arenaRocks;
    /** An eligible boss part is acquired before ordinary objects on ranged
     * attacks. Opt-in encounter policy; physical contacts and melee are unchanged. */
    bool rangedAimFirst = false;
};

} // namespace gdl::game
