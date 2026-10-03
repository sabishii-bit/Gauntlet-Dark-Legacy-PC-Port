#pragma once

#include <memory>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/players/PlayerImpact.h"
#include "game/world/ItemFigure.h"
#include "game/world/ItemSupport.h"

namespace gdl::game {

/** Someone a trap can hurt. */
struct TrapVictim {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
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
 * one after another and rests again. While it is out of its rest whoever stands in its box
 * is hurt by its record's value, and is then left alone until the sequence it was caught in
 * has run out twice over, as the original leaves them. The level scales the times and the
 * damage.
 */
class Traps {
public:
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
        u32 properties = 0;
        s32 action = kResting;
        s32 ticksLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool disarmed = false; ///< still and harmless for good
        bool gone = false;
        ItemFigure figure;
        Obstacle box;
        ItemSupport support;
    };

    /** `timeScale` stretches every rest and `damageScale` every hurt, as the level says.
     * The optional realm archive supplies figures absent from a boss's own archive. */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, u32 seed = 1, f32 timeScale = 1.0f,
              f32 damageScale = 1.0f, ItemArchive* realmItems = nullptr);
    void clear();
    void syncFloors();
    usize size() const { return m_traps.size(); }
    const Trap& trap(usize index) const { return *m_traps[index]; }
    /** Traps that left their resting sequence during the latest update. */
    std::span<const usize> wakes() const { return m_wakes; }
    void setPlayerCount(s32 players);
    /** Whether a trap is out of its rest, and hurts. */
    bool armed(usize index) const { return m_traps[index]->action != kResting; }

    /** Potion magic's lesser valkyrie perk (fn_8005BA1C): back to its rest and held there
     * 600 ticks. True when it shows, which it does not for a trap held within sixty ticks
     * (540 or more left). */
    bool stop(usize index);
    /** The greater: the trap's `<name>_D` figure stands in its place still and harmless for
     * good, or without one it goes. False for a trap already disarmed. */
    bool disarm(usize index, RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                const WorldCollision* collision, ItemArchive* realmItems = nullptr);

    std::vector<TrapHit> update(s32 ticks, f32 seconds, std::span<const TrapVictim> party,
                                bool timeStopped = false);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr,
              TreeModel::Pass pass = TreeModel::Pass::All) const;

private:
    s32 restTicks(const Trap& trap);

    std::vector<std::unique_ptr<Trap>> m_traps;
    std::vector<usize> m_wakes;
    std::vector<f32> m_gaps; ///< per victim, seconds before they can be hurt again
    std::minstd_rand m_random{1};
    f32 m_timeScale = 1.0f;
    const WorldCollision* m_collision = nullptr;
};

} // namespace gdl::game
