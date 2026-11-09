#pragma once

#include <optional>
#include <vector>

#include "expansions/shared/level/LegendsFixtureEvents.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"

namespace gdl::game {

/** Native duration/radius and one-hit admission, feeding the normal GDL combat
 * resolvers. No separate enemy or player health is owned by this adapter. */
class LegendsBlast {
public:
    struct Area {
        Vec3 position{0};
        f32 radius = 0;
        f32 damage = 0;
    };
    explicit LegendsBlast(LegendsFixtureEvent event, f32 damageScale = 1);
    std::optional<Area> step(f32 seconds);
    std::optional<f32> touch(const LegendsPortalVisitor& visitor);
    f32 damageAt(const Vec3& position) const;
    /** Native barrel thresholds ignore the elemental half of a packed hit. */
    f32 physicalDamageAt(const Vec3& position) const;
    const LegendsFixtureEvent& event() const { return m_event; }
    bool alive() const { return m_elapsed < m_event.seconds; }
    std::vector<s32>& reachedOpponents() { return m_opponents; }
    std::vector<s32>& reachedFixtures() { return m_fixtures; }

private:
    LegendsFixtureEvent m_event;
    f64 m_elapsed = 0;
    f32 m_radius = 0;
    f32 m_physical = 0, m_elemental = 0;
    u8 m_players = 0;
    std::vector<s32> m_opponents, m_fixtures;
};

} // namespace gdl::game
