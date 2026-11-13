#pragma once

#include <functional>
#include <optional>
#include <span>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"

#include "game/enemies/PlacedEnemies.h"

namespace gdl::game {

class Critters;
class CritterStatues;

/** Native placement resolution, shared by live and private resource owners. */
class OpponentPlacements {
public:
    using LoadKind = std::function<bool(s32)>;
    using ClaimItem = std::function<std::optional<usize>(const Vec3&, f32, f32)>;
    /** Load time only. Resolves statues and carried pickups in authored order;
     * the returned immutable roster includes every remaining native family. A placement
     * stands when the `party` the level is set up for meets its minimum (ItemVisible); IT
     * waits for company among the players `present` (fn_80060114). */
    static std::vector<PlacedEnemies::Placement>
    load(RenderDevice& device, const WorldLayout& layout, std::span<const LevelEnemy> roster,
         s32 party, s32 present, Enemies& enemies, Critters& critters, CritterStatues& statues,
         const WorldCollision* collision, const ClaimItem& claim);
    static std::optional<PlacedEnemies::Placement>
    ordinary(const ItemInstance& instance, const ItemInfo& info, std::span<const LevelEnemy> roster,
             s32 party, s32 present, const LoadKind& load);
};

} // namespace gdl::game
