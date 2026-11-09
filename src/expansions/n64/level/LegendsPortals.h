#pragma once

#include "expansions/n64/formats/LegendsN64Portal.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"

namespace gdl::game {

/** Cylinder/cylinder contact, with the same feet/height convention as native
 * player bodies. Invalid visitors are never admitted to an exit. */
bool touchesLegendsPortal(const formats::LegendsN64Portal& portal,
                          const LegendsPortalVisitor& visitor);

inline constexpr f32 kLegendsPortalDepartureSeconds = 2.0f;

struct LegendsPortalDeparture {
    f32 yaw = 0;   ///< additional radians about the player's feet
    f32 scale = 1; ///< relative to the character's existing authored scale
    bool finished = false;
};

/** N64 player substate0x12: spin and shrink in place for two seconds. There is
 * no vertical displacement, unlike Dark Legacy's sinking departure. */
LegendsPortalDeparture legendsPortalDeparture(f32 seconds);

/** Native subtype5's 30Hz readiness clock. All living players must share an
 * exit; a changed occupancy mask restores at least two seconds of waiting.
 * This only emits a one-shot departure request. The caller owns the common
 * player departure animation, loading transition and eventual scene switch. */
class LegendsPortalMotion {
public:
    enum class Phase { Idle, Starting, Active, Closing, Departed };
    explicit LegendsPortalMotion(u32 startingFrames = 0, bool hasStarting = false,
                                 bool hasClosing = false);
    bool advance(f32 seconds, u8 occupied, u8 living, bool enabled = true);
    /** Losing permission discards all readiness, including partial 30Hz time. */
    void reset();
    void finishPresentation();
    Phase phase() const { return m_phase; }
    u32 generation() const { return m_generation; }
    s32 waitingTicks() const { return m_waiting; } ///< native 30Hz ticks

private:
    bool tick(u8 occupied, u8 living);
    Phase m_phase = Phase::Idle;
    u32 m_startingFrames = 0;
    u32 m_generation = 0;
    s32 m_waiting = 0;
    u8 m_previousOccupants = 0;
    bool m_hasStarting = false;
    bool m_hasClosing = false;
    f64 m_accumulator = 0;
};

} // namespace gdl::game
