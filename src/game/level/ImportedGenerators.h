#pragma once

#include <array>

#include "engine/assets/WorldLayout.h"

namespace gdl::game {

/** Native placement translated to the existing GDL brood/AI rules. Artwork stays
 * with the imported level; native indices never index the GameCube item tables. */
struct ImportedGenerator {
    u32 nativeIndex = 0;
    s32 kind = 0;
    s32 tier = 1;
    Vec3 position{0};
    f32 yaw = 0;
    ItemInfo body;
    s32 minimumPlayers = 0;
    /** Optional final whole hit points for states 1..3. All zero retains the
     * ordinary equal-state GDL rule; otherwise live states must be positive. */
    std::array<s32, 3> stageHealth{};
    /** Optional native mixed brood, independently of the generator damage state.
     * Negative entries mean no override. Every entry must be a valid swarm kind
     * when broodTier is nonzero; selection occurs once per attempted birth. */
    std::array<s32, 3> broodKinds{-1, -1, -1};
    s32 broodTier = 0;
};

struct ImportedGeneratorState {
    u32 nativeIndex = 0;
    s32 pool = -1;
    s32 state = 0;
    Vec3 position{0};
    bool standing = false;
};

} // namespace gdl::game
