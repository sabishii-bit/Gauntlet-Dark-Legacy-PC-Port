#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

struct LegendsPickup;

/** Native e00649dc's missed-collectible departure cue. Party ownership, not
 * whether a repeat-visit object was touched, determines whether it is missing. */
std::optional<u32> legendsN64MissedRelicCue(std::span<const LegendsPickup> pickups,
                                            std::span<const LegendsProgress> party);

/** Native e0062ca0 stage departure overrides take priority over missed relics.
 * Admission is the caller's responsibility; console jumps do not play them. */
std::optional<u32> legendsN64DepartureCue(std::string_view map,
                                          std::span<const LegendsPickup> pickups,
                                          std::span<const LegendsProgress> party);

struct LegendsSumnerChoice {
    u32 textId = 0;
    u32 action = 0;
};

struct LegendsSumnerVisitor {
    s32 seat = -1;
    bool touching = false;
    bool alive = false;
};

/** Native Tower podium menu and party-wide hint selection. These are text-only
 * advice rows, independent of the Sumner speech queue and GDL campaign state. */
class LegendsSumner {
public:
    static LegendsSumner decode(std::span<const u8> program, std::span<const u8> tower);
    bool ready() const { return !m_choices.empty(); }
    u32 titleId() const { return m_title; }
    std::span<const LegendsSumnerChoice> choices() const { return m_choices; }

    /** Native actions 0xd=World Hint, 0x2e=Rune Hint, 0x2f=Enemy Hint.
     * Continue (0x11), Shop (0x15), and History (2) are routed by the scene. */
    std::optional<u32> adviceTextId(u32 action, std::span<const LegendsProgress> party) const;

    /** History replays the last displayed Tower milestone, not the latest
     * collectible. Initially that state is 14; the scene retains subsequent
     * milestones. Returned numbers use loadLegendsN64Scroll's native advice path. */
    std::optional<u8> historyPage(u32 lastMilestone, std::span<const LegendsProgress> party) const;
    /** Resume the latest native advice chapter from independent campaign state. */
    u32 milestone(std::span<const LegendsProgress> party) const;

    /** Feed the movement request before the podium refuses it. Reuses the same
     * collision response, so nearby Sumner-body contacts do not open the menu. */
    bool podiumContact(const Vec3& from, const Vec3& requested, f32 radius, f32 height) const;

    /** Returns the lowest living contacting seat after the native >1s cooldown.
     * A paused/modal scene freezes the game clock and cannot admit a visitor. */
    std::optional<s32> update(f32 seconds, std::span<const LegendsSumnerVisitor> visitors,
                              bool blocked = false);

private:
    struct Rule {
        u32 kind = 0;
        u32 identity = 0;
        u32 textId = 0;
        std::string map;
    };
    u32 m_title = 0;
    std::vector<LegendsSumnerChoice> m_choices;
    std::array<std::vector<Rule>, 3> m_rules;
    formats::LegendsN64TowerVolume m_podium;
    std::string m_firstMountain;
    f64 m_sincePrompt = 0;
};

} // namespace gdl::game
