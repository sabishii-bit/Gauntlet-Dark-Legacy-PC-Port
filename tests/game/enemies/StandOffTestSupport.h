#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/critters/CombatantKind.h"
#include "game/enemies/LevelOpponents.h"
#include "game/players/PlayerActor.h"

namespace gdl::test {
/** A great one as a level stands it: its family, the level's realm and the archive it needs. */
struct StandingGreatOne {
    game::CombatantKind kind = game::CombatantKind::Golem;
    char realm = 'G';
    const char* archive = "";
};

/** Where `actor`, walking on from where it stands a `step` at a time, is first stopped by the
 * level's opponents; nothing when 240 steps go by without one. */
inline std::optional<Vec3> walkedInto(const game::LevelOpponents& opponents,
                                      const game::PlayerActor& actor,
                                      const Vec3& step = {0, 0, -0.25f}) {
    constexpr s32 kMostSteps = 240;
    Vec3 stop = actor.position();
    for (s32 i = 0; i < kMostSteps; ++i) {
        const Vec3 wanted = stop + step;
        stop = opponents.resolveMovement(actor, stop, wanted);
        if (glm::distance(stop, wanted) > 1e-4f) {
            return stop;
        }
    }
    return std::nullopt;
}
} // namespace gdl::test
