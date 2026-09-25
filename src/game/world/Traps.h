#pragma once

#include <memory>
#include <random>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

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
    static constexpr s32 kTicksPerTimeUnit = 2; ///< the record's times are in half ticks

    /** One trap. */
    struct Trap {
        s32 instance = -1;
        f32 damage = 0.0f;
        s32 offTime = 0; ///< the record's, in its own units
        s32 subtype = 0;
        s32 action = kResting;
        s32 ticksLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        ItemFigure figure;
        Obstacle box;
    };

    /** `timeScale` stretches every rest and `damageScale` every hurt, as the level says.
     * The optional realm archive supplies figures absent from a boss's own archive. */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, u32 seed = 1, f32 timeScale = 1.0f,
              f32 damageScale = 1.0f, ItemArchive* realmItems = nullptr);
    void clear();
    usize size() const { return m_traps.size(); }
    const Trap& trap(usize index) const { return *m_traps[index]; }
    void setPlayerCount(s32 players);
    /** Whether a trap is out of its rest, and hurts. */
    bool armed(usize index) const { return m_traps[index]->action != kResting; }

    std::vector<TrapHit> update(s32 ticks, f32 seconds, std::span<const TrapVictim> party);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;

private:
    s32 restTicks(const Trap& trap);

    std::vector<std::unique_ptr<Trap>> m_traps;
    std::vector<f32> m_gaps; ///< per victim, seconds before they can be hurt again
    std::minstd_rand m_random{1};
    f32 m_timeScale = 1.0f;
};

} // namespace gdl::game
