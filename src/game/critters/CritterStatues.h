#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

#include "game/combat/MissileTarget.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {
/** The golems, gargoyles and Deaths a level places stand as item statues, the `GOL_STATUE` or
 * `GAR_STATUE` tree of the great one's own archive where the placement is (SetItem, items.c
 * 6798), until woken: by a player within its sight (fn_8005D730), by a blow (fn_8005C1DC) or
 * by a trigger flagged to wake the nearest (fn_800606FC). Woken and in view, a statue plays
 * its ACTIVE sequence and, once that has run its ticks, the great one stands in its place
 * (fn_80060114). The archives are borrowed and must outlive the statues. */
class CritterStatues {
public:
    CritterStatues() = default;
    CritterStatues(const CritterStatues&) = delete;
    CritterStatues& operator=(const CritterStatues&) = delete;
    CritterStatues(CritterStatues&&) = delete;
    CritterStatues& operator=(CritterStatues&&) = delete;
    ~CritterStatues() = default;
    /** A placement standing as a statue, and what it becomes. */
    struct Placement {
        CombatantKind kind = CombatantKind::Unknown;
        /** Death is a swarm enemy, not a CRITTER family. Its spawn is held until
         * the placed DEATHSTATUE1/2 item has been awakened. */
        std::optional<EnemySpawn> enemy;
        std::string form;      ///< a gargoyle's ("GAR_EAGL"); empty for the default
        ItemInstance instance; ///< where the level puts it
        bool onFloor = true;   ///< stood on the floor beneath, unless its record keeps its height
        f32 radius = 0.0f;     ///< the record's solid body, independent of the wake range
        f32 height = 0.0f;
        f32 viewRadius = 0.0f; ///< twice the record's larger size, for coming into view
        f32 sight = 0.0f;      ///< approach wake radius; negative disables proximity waking
        s32 activeOn = 0; ///< the record's, in half ticks; nought for the sequence's own length
        std::optional<usize> carried; ///< the pickup the great one holds
        s32 support = -1;     ///< AddItemSub's supporting world node, including moving decks
        Mat4 supportLocal{1}; ///< fixed placement relative to that node
        bool operator==(const Placement& /*other*/) const;
        friend void visitNativeState(NativeStateDigest& digest, const Placement& value);
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
    /** Carry idle and rising statues without advancing their activation clocks. */
    void syncFloors();
    /** Bounds for the native 64-cell-wide item query preceding proximity waking.
     * Unconfigured/degenerate bounds leave the query in one cell. */
    void setContactGridBounds(const Vec3& minimum, const Vec3& maximum);
    usize count() const { return m_sim.statues.size(); }
    /** Stable presentation identity, independent of the compacted collision list. */
    u64 instanceOf(usize index) const { return m_sim.statues[index]->instance; }
    const ItemFigure& figure(usize index) const { return m_sim.statues[index]->figure; }
    ItemArchive& archive(usize index) const { return *m_sim.statues[index]->archive; }
    const Placement& placement(usize index) const { return m_sim.statues[index]->placement; }
    const Vec3& positionOf(usize index) const { return m_sim.statues[index]->figure.position(); }
    bool woken(usize index) const { return m_sim.statues[index]->woken; }
    /** Whether its ACTIVE sequence is under way. */
    bool rising(usize index) const { return m_sim.statues[index]->rising; }
    void wake(usize index);
    /** The statue not yet woken nearest `spot`, and how far off it is. */
    std::optional<std::pair<usize, f32>> nearestAsleep(const Vec3& spot) const;
    /** Wakes statues within their placement's sight and resolves solid body contact. */
    Vec3 touch(const Vec3& position, f32 radius, f32 height = 0.0f);
    struct Visitor {
        Vec3 position{0};
        f32 radius = 0;
        f32 height = 0;
    };
    /** No current or prospective activation, and no body correction, for this
     * complete party. A caller may then leave dormant statue owners untouched. */
    bool dormantFor(std::span<const Visitor> visitors) const;
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
        u64 instance = 0;
        ItemArchive* archive = nullptr;
        Placement placement;
        ItemFigure figure;
        bool woken = false;
        bool rising = false;
        s32 ticksLeft = 0;
        bool operator==(const Statue& other) const;
        friend void visitNativeState(NativeStateDigest& digest, const Statue& value);
    };
    static Obstacle obstacleOf(const Statue& statue);
    static bool wakesAt(const Statue& statue, const Vec3& position, f32 radius, f32 height);
    s32 contactCell(f32 coordinate, f32 origin) const;
    bool inContactGrid(const Vec3& statue, const Vec3& player, f32 radius) const;
    struct Prototype {
        ItemArchive* archive = nullptr;
        std::unique_ptr<ItemFigure> figure;
    };
    std::vector<Prototype> m_prototypes;
    const WorldCollision* m_collision = nullptr; ///< borrowed for the stage lifetime
    struct Simulation {
        std::vector<std::optional<Statue>> statues;
        std::vector<Placement> risen;
        Vec2 contactGridOrigin{0};
        f32 contactGridInverseWidth = 0;
        u64 nextInstance = 1; ///< not reset by clear; old snapshots cannot reuse a statue's ID
        bool operator==(const Simulation&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& value);
    };
    friend void visitNativeState(NativeStateDigest& digest, const Statue& value);
    friend void visitNativeState(NativeStateDigest& digest, const Simulation& value);

public:
    /** Value-owned live figures and queued births; loaded prototypes stay bound. */
    class LocalSnapshot {
        friend class CritterStatues;
        Simulation m_values;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    /** Archives must already have stage-scoped semantic identities. Also indexes
     * retained prototype art after a statue has risen and left the active list. */
    bool registerNativeResources(NativeStateResources& resources) const;
    /** Actual captured values, including queued births and each figure's own
     * clocks/particles. Shared model draw caches and retained prototypes are not state. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    static bool archivesLoaded(const Simulation& state);
    static bool appendStateDigest(NativeStateDigest& digest, const Simulation& state);
    Simulation m_sim;
    u64 m_localEpoch = 1;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
