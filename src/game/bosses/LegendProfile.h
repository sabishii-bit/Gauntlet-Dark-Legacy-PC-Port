#pragma once

#include <optional>
#include <string>

#include "game/bosses/LegendItems.h"

namespace gdl::game {
/** Optional immutable bindings for another game's legend rite: its authored art, and where its
 * rite wakes the boss. The ordinary GDL rite still controls gestures, impact and weakening. */
struct LegendProfile {
    std::string held;
    std::string projectile;
    std::string burst;
    std::string secondBurst;
    std::optional<LegendShow::Flight> flight;
    Vec3 bossOffset{0};
    bool charge = false;
    bool trail = false;
    /** Bound tree index, not a model/drawable name. Imported skeletal name
     * tables may identify an animated node whose mesh changes every frame. */
    std::optional<u32> targetNode;
    /** Empty substitutions preserve the imported body's own eye/stump art.
     * A missing profile, rather than an empty string, selects GDL defaults. */
    std::string eyeReplacement;
    std::string lionStump;
    f32 impactAttachmentSeconds = 0; ///< retain the projectile at targetNode after impact
    /** While the rite runs, its boss wakes once its wake deadline passes, wherever the party
     * stands. */
    bool wakesOnDeadline = false;
};
} // namespace gdl::game
