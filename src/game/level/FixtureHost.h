#pragma once
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/HurtKind.h"
#include "game/players/PlayerImpact.h"

namespace gdl::game {
/** What the level's fixtures report into as they spring, open and burst: harm, lessons,
 * cards, opponent damage and what a chest or barrel lets out. Answered at once; kept by
 * nobody. */
class FixtureHost {
public:
    virtual ~FixtureHost() = default;
    /** Harm to party member `member`; `directed` is whether it came from a direction. */
    virtual void hurtMember(usize member, f32 damage, HurtKind kind, bool directed,
                            const PlayerImpact& impact) = 0;
    /** Whether lesson `id` went up over party member `member`. */
    virtual bool lesson(s32 id, usize member) = 0;
    /** A pickup card `name` over the player at input slot `seat`. */
    virtual void card(s32 seat, std::string_view name) = 0;
    /** A blast's ring reaching `radius` about `position` with `damage`: the swarm, the great
     * ones, the boss and the generators within it and not yet in `reached` (the blast's
     * ledger of the opponents' ids), which a blow over two joins. */
    virtual void blastOpponents(const Vec3& position, f32 radius, f32 damage,
                                std::vector<s32>& reached, u32 flags) = 0;
    /** Whether the enemy of contents record `record`, `count` of it, came out of a chest or
     * barrel at `position`. */
    virtual bool releaseEnemy(s32 record, const Vec3& position, s32 count) = 0;
    /** A bottle of potion `kind` lying at `position` broken by a blast. */
    virtual void shatterPotion(s32 kind, const Vec3& position) = 0;
};
} // namespace gdl::game
