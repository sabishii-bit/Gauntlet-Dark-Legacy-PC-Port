#pragma once

#include <array>
#include <optional>
#include <vector>

#include "engine/assets/WorldLayout.h"

namespace gdl::game {

/** Native placement translated to the existing GDL brood/AI rules. Artwork stays
 * with the imported level; native indices never index the GameCube item tables. */
struct ImportedGenerator {
    /** Optional range-based breeding policy. Population is rounded after the
     * bounded difficulty and bonus are applied; waits interpolate between the
     * two native tick counts. No policy retains the existing GDL/N64 rules. */
    struct Breeding {
        std::array<s32, 3> broodByState{};
        f32 difficulty = 1;
        Vec2 broodDifficulty{1, 1};
        f32 broodBonus = 0;
        s32 maximumBrood = 10;
        std::array<s32, 2> waitTicks{};
        Vec2 waitDifficulty{1, 1};
        f32 activeDistance = 50;
        bool operator==(const Breeding&) const = default;
    };
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
    /** Optional native mixed brood, independently of generator damage state.
     * N64 selects three species; Dreamcast selects four. Empty means ordinary
     * brood. Selection occurs once per attempted birth from at most four kinds. */
    std::vector<s32> broodKinds;
    /** Empty selects each species' common strategy. Otherwise one native
     * strategy per brood kind, including zero's species-specific default. */
    std::vector<s32> broodAlgorithms;
    s32 broodTier = 0;
    s32 algorithm = -1;    ///< common AI strategy; negative selects the species default
    s32 maximumBrood = 0;  ///< zero selects the common tier's population limit
    s32 spawnInterval = 0; ///< native cadence in common movement ticks; zero selects tier default
    bool alwaysActive = false;
    std::optional<Breeding> breeding;
};

struct ImportedGeneratorState {
    u32 nativeIndex = 0;
    s32 pool = -1;
    s32 state = 0;
    Vec3 position{0};
    bool standing = false;
};

} // namespace gdl::game
