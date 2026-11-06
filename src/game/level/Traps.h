#pragma once

#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"
#include "game/players/PlayerImpact.h"

namespace gdl::game {

/** Someone a trap can hurt. */
struct TrapVictim {
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< collision centre, not the feet
    f32 radius = 0.75f;
    f32 halfHeight = 1.5f;
};

/** A trap caught someone this update. */
struct TrapHit {
    usize trap = 0;
    usize victim = 0;
    f32 damage = 0.0f;
    s32 subtype = 0;      ///< which picks the trap's sound
    bool pierces = false; ///< spikes and blades, which a victim groans at
    Vec3 position{0.0f, 0.0f, 0.0f};
    PlayerImpact impact;
};

/**
 * A level's traps (spikes, flames, saw blades), cycled the way the original cycles them: each
 * rests on its first sequence for a time drawn afresh each round (its record's off time,
 * negative meaning at random between half of it and one and a half), then plays its others
 * one after another and rests again. During strike sequences 2 and 4, whoever touches its
 * collision volume is hurt by its record's value, and is left alone until that sequence
 * has run out twice over, as the original leaves them. The level scales the times and the
 * damage.
 */
class Traps : public LocalState<Traps> {
public:
    Traps() = default;
    Traps(const Traps&) = delete;
    Traps& operator=(const Traps&) = delete;
    Traps(Traps&&) noexcept = default;
    Traps& operator=(Traps&&) noexcept = default;
    ~Traps() = default;
    static constexpr s32 kResting = 0;
    static constexpr f32 kSecondsPerTickLeft = 1.0f / 30.0f; ///< a victim's respite
    static constexpr s32 kSpikes = 0;                        ///< the subtypes that pierce
    static constexpr s32 kBlade = 3;
    static constexpr s32 kBlades = 4;
    static constexpr s32 kTentWall = 5;         ///< raised (its first two moves), stops missiles
    static constexpr s32 kTicksPerTimeUnit = 2; ///< the record's times are in half ticks
    static constexpr s32 kStopTimeRest = 30;    ///< held safe, then waits half a second
    static constexpr std::string_view kDisarmedSuffix = "_D";
    static constexpr s32 kStoppedRest = 600;    ///< ticks potion magic holds a trap at rest
    static constexpr s32 kStopShownUnder = 540; ///< no effect for one held within 60 ticks

    /** One trap. */
    struct Trap {
        s32 instance = -1;
        f32 damage = 0.0f;
        s32 offTime = 0; ///< the record's, in its own units
        s32 subtype = 0;
        std::string name; ///< the record's, which its sound may go by
        u32 properties = 0;
        s32 action = kResting;
        s32 ticksLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool disarmed = false; ///< still and harmless for good
        bool gone = false;
        bool rectangularContact = false; ///< native coltype 3 expands each axis by the body radius
        ItemFigure figure;
        Obstacle box;
        ItemSupport support;
        bool operator==(const Trap&) const = default;
        template <class Visitor> friend void visitNativeState(Visitor& visitor, const Trap& value) {
            visitor.template fields<16>(value);
        }
    };

    /** `timeScale` stretches every rest and `damageScale` every hurt, as the level says.
     * The scope supplies figures a boss's own archive lacks from the realm's. */
    bool bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
              const WorldCollision* collision, u32 seed = 1, f32 timeScale = 1.0f,
              f32 damageScale = 1.0f);
    void clear();
    void syncFloors();
    usize size() const { return m_sim.traps.size(); }
    const Trap& trap(usize index) const { return m_sim.traps[index]; }
    /** Traps that left their resting sequence during the latest update. */
    std::span<const usize> wakes() const { return m_sim.wakes; }
    void setPlayerCount(s32 players);
    /** Whether a trap is out of its rest (including harmless wind-up and recovery). */
    bool armed(usize index) const { return m_sim.traps[index].action != kResting; }

    /** Potion magic's lesser valkyrie perk (fn_8005BA1C): back to its rest and held there
     * 600 ticks. True when it shows, which it does not for a trap held within sixty ticks
     * (540 or more left). */
    bool stop(usize index);
    /** The greater: the trap's `<name>_D` figure stands in its place still and harmless for
     * good, or without one it goes. False for a trap already disarmed. */
    bool disarm(usize index, RenderDevice& device, const WorldLayout& layout,
                const AssetScope& scope, const WorldCollision* collision);

    std::vector<TrapHit> update(s32 ticks, f32 seconds, std::span<const TrapVictim> party,
                                bool timeStopped = false);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr, TreeModel::Pass pass = TreeModel::Pass::All,
              f32 presentationAlpha = -1.0f) const;
    void capturePresentation();
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    s32 restTicks(const Trap& trap);
    static bool touches(const Trap& trap, const TrapVictim& victim);

    struct Simulation {
        std::vector<Trap> traps;
        std::vector<usize> wakes;
        std::vector<f32> gaps; ///< per victim, seconds before they can be hurt again
        std::minstd_rand random{1};
        f32 timeScale = 1.0f;
        bool operator==(const Simulation&) const = default;
    } m_sim;

    friend class LocalState<Traps>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Simulation;
    struct LocalChildren {};
    static constexpr auto localParts() { return local::parts(local::own()); }
    const LocalValues& localValues() const { return m_sim; }
    LocalValues& localValues() { return m_sim; }
    /** Writes the schema, what the traps were bound to and `state`. */
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& state) const;
    const WorldCollision* m_collision = nullptr;
    const WorldLayout* m_layout = nullptr;
    AssetScope m_scope;
    LocalBinding m_localBinding;
};

} // namespace gdl::game
