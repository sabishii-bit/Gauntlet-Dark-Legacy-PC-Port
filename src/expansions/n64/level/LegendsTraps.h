#pragma once

#include <optional>

#include "expansions/n64/formats/LegendsN64Traps.h"
#include "expansions/shared/level/LegendsFixtureEvents.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** N64 e004ba94 uses proportional armor, not GDL's flat subtraction. */
f32 legendsTrapDamage(f32 damage, f32 armorAttribute);

/** Native trap timing/presentation with no second character or damage system.
 * The caller feeds actual animation completion and applies returned hits through
 * the ordinary player combat pipeline. */
class LegendsTrap {
public:
    enum class Phase { Idle, Starting, Active, Ending };
    LegendsTrap(formats::LegendsN64Trap definition, f32 damageScale, u32 activeSequenceFrames,
                u32 startingSequenceFrames = 0, bool hasEnding = false);
    void advance(f32 seconds, bool visible, bool timeStopped, bool animationFinished);
    std::optional<LegendsTrapHit> touch(const LegendsPortalVisitor& visitor);
    void placeAt(const Vec3& feet);
    /** Tick-pose attachment only. Missing named art disables contact, not a fallback box. */
    void nodeContact(std::optional<Vec3> position);
    const formats::LegendsN64Trap& definition() const { return m_definition; }
    Phase phase() const { return m_phase; }
    u32 generation() const { return m_generation; }
    bool dangerous() const {
        return m_hazardTicks > 0 && !m_stopped &&
               (m_definition.contactNode.empty() || m_nodeContact.has_value());
    }
    Obstacle contact() const;

private:
    void begin();
    void recover();
    formats::LegendsN64Trap m_definition;
    f32 m_scale = 1;
    u32 m_activeFrames = 0, m_startingFrames = 0;
    bool m_hasEnding = false, m_stopped = false;
    Phase m_phase = Phase::Idle;
    f64 m_accumulator = 0;
    s32 m_waiting = 0, m_hazardTicks = 0;
    u32 m_random = 1, m_generation = 0;
    u8 m_hitPlayers = 0;
    std::optional<Vec3> m_nodeContact;
};

} // namespace gdl::game
