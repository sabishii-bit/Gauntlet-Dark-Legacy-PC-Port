#pragma once

#include <array>
#include <optional>
#include <random>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/shared/level/LegendsFixtureEvents.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"
#include "formats/WorldFile.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** Dreamcast's authored spike and flame cycles. Native records are decoded directly;
 * the common player combat pipeline still owns health, armor and immunities. */
class LegendsDreamcastTrap {
public:
    static std::optional<LegendsDreamcastTrap>
    decode(const formats::ItemInfoRecord& info, const formats::ItemInstanceRecord& instance,
           u32 nativeIndex, std::array<s32, 4> sequenceTicks, std::string* diagnostic = nullptr);
    void advance(f32 seconds, bool timeStopped);
    std::optional<LegendsTrapHit> touch(const LegendsPortalVisitor& visitor);
    s32 action() const { return m_action; }
    s32 ticksLeft() const { return m_wait; }
    u32 generation() const { return m_generation; }
    bool dangerous() const { return m_action == 2 && !m_stopped; }
    u32 contactSound(s32 realm) const;

private:
    s32 rest();
    u32 m_nativeIndex = 0;
    Obstacle m_contact;
    Vec3 m_position{0};
    f32 m_damage = 0;
    u32 m_properties = 0;
    s32 m_subtype = 0;
    std::array<s32, 4> m_durations{};
    std::array<f32, 4> m_cooldowns{};
    std::minstd_rand m_random{1};
    f64 m_ticks = 0;
    s32 m_off = 0, m_action = 0, m_wait = 0;
    u32 m_generation = 0;
    bool m_stopped = false;
};

} // namespace gdl::game
