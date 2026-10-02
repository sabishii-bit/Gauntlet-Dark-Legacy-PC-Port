#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** The golems, gargoyles and Deaths a level places stand as item statues, the `GOL_STATUE` or
 * `GAR_STATUE` tree of the great one's own archive where the placement is (SetItem, items.c
 * 6798), until woken: by a player walking into one (fn_8005D730), by a blow (fn_8005C1DC) or
 * by a trigger flagged to wake the nearest (fn_800606FC). Woken and in view, a statue plays
 * its ACTIVE sequence and, once that has run its ticks, the great one stands in its place
 * (fn_80060114). The archives are borrowed and must outlive the statues. */
class CritterStatues {
public:
    /** A placement standing as a statue, and what it becomes. */
    struct Placement {
        CombatantKind kind = CombatantKind::Unknown;
        /** Death is a swarm enemy, not a CRITTER family. Its spawn is held until
         * the placed DEATHSTATUE1/2 item has been awakened. */
        std::optional<EnemySpawn> enemy;
        std::string form;      ///< a gargoyle's ("GAR_EAGL"); empty for the default
        ItemInstance instance; ///< where the level puts it
        f32 radius = 0.0f;     ///< the record's: what stops a player, and where a touch counts
        f32 height = 0.0f;
        f32 viewRadius = 0.0f; ///< twice the record's larger size, for coming into view
        f32 sight = 0.0f;      ///< the placement's sight parameter: under nought, no touch wakes it
        s32 activeOn = 0; ///< the record's, in half ticks; nought for the sequence's own length
        std::optional<usize> carried; ///< the pickup the great one holds
    };
    /** Whether a sphere of a radius about a point is in view. */
    using Seen = std::function<bool(const Vec3&, f32)>;
    static constexpr std::string_view kGolemTree = "GOL_STATUE";
    static constexpr std::string_view kGargoyleTree = "GAR_STATUE";
    static constexpr s32 kIdleSequence = 0;
    static constexpr s32 kActiveSequence = 1;
    static constexpr s32 kTicksPerActiveOn = 2; ///< the record's activeOn is in half ticks
    static constexpr f32 kFramesPerRateUnit = 1.0f / 30.0f; ///< a sequence's rate over thirty

    /** How long a statue's ACTIVE sequence of `frames` at `rate` holds it (ProcessItems'
     * activetime: the frames at the rate over thirty, rounded, in half ticks). */
    static s32 activeTicks(s32 frames, s32 rate);

    /** The statue tree a kind stands as; empty for a kind that stands as itself. */
    static std::string_view treeOf(CombatantKind kind);
    /** Stands a statue for `placement` from `archive`; false when the archive has no statue
     * tree, in which case the placement stands as itself. */
    bool add(RenderDevice& device, ItemArchive& archive, const Placement& placement,
             const WorldCollision* collision);
    void clear();
    usize count() const { return m_statues.size(); }
    const Placement& placement(usize index) const { return m_statues[index]->placement; }
    const Vec3& positionOf(usize index) const { return m_statues[index]->figure.position(); }
    bool woken(usize index) const { return m_statues[index]->woken; }
    /** Whether its ACTIVE sequence is under way. */
    bool rising(usize index) const { return m_statues[index]->rising; }
    void wake(usize index);
    /** The statue not yet woken nearest `spot`, and how far off it is. */
    std::optional<std::pair<usize, f32>> nearestAsleep(const Vec3& spot) const;
    /** A body of `radius` at `position` walking into one: pushed out by the record's
     * radius, and the statue woken when its placement allows. Where the body stands. */
    Vec3 touch(const Vec3& position, f32 radius);
    /** What the statues put in the way: an upright cylinder each. */
    std::vector<Obstacle> obstacles() const;
    /** What a blow can strike, by the statue's place in the list. */
    std::vector<MissileTarget> targets() const;
    /** Plays the woken statues in view on, and takes those whose ACTIVE has run its ticks
     * off the list for `takeRisen`. */
    void update(s32 ticks, f32 seconds, const Seen& seen);
    std::vector<Placement> takeRisen();
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;

private:
    struct Statue {
        Placement placement;
        ItemFigure figure;
        bool woken = false;
        bool rising = false;
        s32 ticksLeft = 0;
    };
    static Obstacle obstacleOf(const Statue& statue);
    std::vector<std::unique_ptr<Statue>> m_statues;
    std::vector<Placement> m_risen;
};
} // namespace gdl::game
