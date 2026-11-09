#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** Dreamcast's post-boss Sumner apparition (1ST_READ.BIN 8c06f800).
 * This is presentation only: the encounter owns rewards and the scene transition.
 * The three speech groups are defeat, rune count, and the remaining shard quest. */
class LegendsDreamcastBossCeremony {
public:
    enum class Stage { None, Waiting, Appearing, Defeat, Runes, Quest, Speech, Leaving, Done };
    struct Group {
        std::vector<std::string> keys;
        std::optional<u32> voice;
        s32 pageHoldTicks = 30;
        s32 afterTicks = 60;
    };
    struct Caption {
        usize group = 0;
        usize page = 0;
        usize shown = 0;
    };
    struct Events {
        std::optional<u32> voice;
        bool appeared = false;
        bool turnToNextPlayer = false;
        bool depart = false;
        bool sparkle = false;
        bool finished = false;
    };

    /** Native IDs 26..33, realm IDs 1..4. Progress is the live party union,
     * in Dreamcast's own bit domain (not the GameCube campaign). */
    bool begin(s32 boss, s32 realm, u64 partyRunes, u64 partyShards);
    void clear();
    /** Advance the adapter's fixed 30 Hz update in native sixtieths. Arbitrary positive
     * chunks are accumulated into two-tick updates; no wall clock is consulted.
     * Supply the current group's localized pages. Speech prevents departure,
     * never the caption timer. There is deliberately no button-skip input. */
    Events step(s32 ticks, std::span<const std::string> pages, bool speechPlaying);

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Done; }
    bool finished() const { return m_stage == Stage::Done; }
    f32 wizardAlpha() const;
    bool allRunes() const { return m_allRunes; }
    s32 boss() const { return m_boss; }
    s32 ticksLeft() const { return m_ticksLeft; }
    const std::array<Group, 3>& groups() const { return m_groups; }
    std::optional<Caption> caption() const { return m_caption; }
    usize groupIndex() const;

    /** 8c06f4c0 averages the defeated boss anchor with participating players;
     * the first player's floor height replaces the boss's height contribution. */
    static Vec3 wizardPosition(s32 boss, const Vec3& bossAnchor, std::span<const Vec3> players);
    static constexpr Vec3 kRunestonePosition{0, -10, 17};
    static constexpr Vec3 kAttentionOffset{0, 12.5f, 0};
    static constexpr f32 kCaptionScale = 0.667f;
    static constexpr f32 kCaptionY = 16.0f;
    static constexpr f32 kCaptionLineHeight = 20.0f;

private:
    void startGroup(usize group, Events& events);
    bool typePage(std::span<const std::string> pages, Events& events);
    void tick(std::span<const std::string> pages, bool speechPlaying, Events& events);

    Stage m_stage = Stage::None;
    s32 m_boss = -1;
    bool m_allRunes = false;
    s32 m_pendingTicks = 0;
    s32 m_ticksLeft = 0;
    s32 m_transparency = 255;
    s32 m_typeTicks = 0;
    s32 m_holdTicks = 0;
    bool m_pageDone = false;
    bool m_groupDone = false;
    std::array<Group, 3> m_groups;
    std::optional<Caption> m_caption;
};

} // namespace gdl::game
