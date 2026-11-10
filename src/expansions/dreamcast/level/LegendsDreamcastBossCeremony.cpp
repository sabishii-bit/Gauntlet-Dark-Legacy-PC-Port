#include "expansions/dreamcast/level/LegendsDreamcastBossCeremony.h"

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <string_view>

namespace gdl::game {
namespace {
// AudioGoodWizard, 8c0e13c0: seven bank/sample tokens per boss at 8c127dcc.
// The two unused boss slots deliberately have no recorded Dreamcast speech.
constexpr u32 kSilent = 0xffffffffU;
constexpr std::array<std::array<u32, 7>, 8> kVoices{{
    {0x27000a, 0x27000b, 0x27000c, 0x27000d, 0x27000e, 0x27000f, 0x270010},
    {0x280018, 0x280019, 0x28001a, 0x28001b, 0x28001c, 0x28001d, 0x28001e},
    {0x29000e, 0x29000f, 0x290010, 0x290011, 0x290012, 0x290013, 0x290014},
    {0x2a0013, 0x2a0014, 0x2a0015, 0x2a0016, 0x2a0017, 0x2a0018, 0x2a0019},
    {kSilent, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent},
    {kSilent, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent},
    {0x2c0015, 0x2c0016, 0x2c0017, kSilent, kSilent, kSilent, kSilent},
    {0x2e0014, 0x2e0014, kSilent, kSilent, kSilent, kSilent, kSilent},
}};

std::optional<u32> voice(usize boss, usize group) {
    const u32 token = kVoices[boss][group];
    return token == kSilent ? std::nullopt : std::optional{token};
}

std::vector<std::string> keys(std::string_view group, usize pages) {
    std::vector<std::string> result;
    result.reserve(pages);
    for (usize page = 0; page < pages; ++page) {
        result.push_back(std::format("legends.dreamcast.victory.{}.{}", group, page));
    }
    return result;
}
} // namespace

bool LegendsDreamcastBossCeremony::begin(s32 boss, s32 realm, u64 partyRunes, u64 partyShards) {
    if (running() || boss < 26 || boss > 33) {
        return false;
    }
    clear();
    m_boss = boss;
    m_allRunes = boss == 32 && (partyRunes & 0xfffU) == 0xfffU;
    const auto index = static_cast<usize>(boss - 26);
    constexpr std::array<std::string_view, 8> kDefeat{"dragon", "chimera", "djinn",   "drider",
                                                      "plague", "yeti",    "skorne1", "skorne2"};
    m_groups[0] = {keys(kDefeat[index], boss == 32 ? 2 : (boss == 33 ? 6 : 1)), voice(index, 0),
                   boss == 33 ? 45 : 30, 30};
    if (boss <= 31) {
        // 8c06f85e: realm 1..4 has three adjacent rune bits. Outside those
        // realms the original leaves the full mask intact before categorizing it.
        const u64 runes =
            realm >= 1 && realm <= 4 ? ((partyRunes >> (3 * (realm - 1))) & 7U) : partyRunes;
        const usize count = runes == 0 ? 0 : (runes == 7 ? 3 : (std::popcount(runes) == 1 ? 1 : 2));
        m_groups[1] = {keys(std::format("runes{}", count), 1), voice(index, count + 1), 30, 60};
        const bool allShards = (partyShards & 0x1eU) == 0x1eU;
        m_groups[2] = {keys(allShards ? "all_shards" : "remaining_shards", 2),
                       voice(index, allShards ? 6 : 5), 30, 60};
    } else if (boss == 32) {
        m_groups[1] = {keys(m_allRunes ? "all_runes" : "remaining_runes", m_allRunes ? 6 : 4),
                       voice(index, m_allRunes ? 2 : 1), 30, 60};
    }
    m_stage = Stage::Waiting;
    // 8c06f900 uses a two-second wait for the first Skorne and five otherwise.
    m_ticksLeft = boss == 32 ? 120 : 300;
    return true;
}

void LegendsDreamcastBossCeremony::clear() {
    *this = LegendsDreamcastBossCeremony{};
}

usize LegendsDreamcastBossCeremony::groupIndex() const {
    return m_stage == Stage::Runes ? 1 : (m_stage == Stage::Quest ? 2 : 0);
}

f32 LegendsDreamcastBossCeremony::wizardAlpha() const {
    if (m_stage == Stage::None || m_stage == Stage::Waiting) {
        return 0;
    }
    return static_cast<f32>(255 - m_transparency) / 255.0f;
}

Vec3 LegendsDreamcastBossCeremony::wizardPosition(s32 boss, const Vec3& bossAnchor,
                                                  std::span<const Vec3> players) {
    if (boss >= 32) {
        return {0, -12, 6};
    }
    Vec3 centre = bossAnchor;
    f32 count = 1;
    for (const Vec3& player : players) {
        centre += player;
        if (count == 1) {
            centre.y = player.y * 2;
        }
        ++count;
    }
    return centre / count + Vec3{0, 3, 0};
}

void LegendsDreamcastBossCeremony::startGroup(usize group, Events& events) {
    constexpr std::array<Stage, 3> kStages{Stage::Defeat, Stage::Runes, Stage::Quest};
    m_stage = kStages[group];
    m_caption = Caption{group, 0, 0};
    m_typeTicks = 0;
    m_holdTicks = 0;
    m_pageDone = false;
    m_groupDone = false;
    events.voice = m_groups[group].voice;
}

bool LegendsDreamcastBossCeremony::typePage(std::span<const std::string> pages, Events& events) {
    const Group& group = m_groups[groupIndex()];
    if (m_groupDone) {
        m_holdTicks += 2;
        return m_holdTicks >= group.afterTicks;
    }
    if (!m_caption || m_caption->page >= group.keys.size()) {
        m_caption.reset();
        m_groupDone = true;
        m_holdTicks = 0;
        // Empty native groups skip directly; a missing translation does not
        // leave the player permanently locked in an unfinishable caption.
        return group.keys.empty();
    }
    const std::string_view page =
        m_caption->page < pages.size() ? pages[m_caption->page] : std::string_view{};
    m_typeTicks += 2;
    if (!m_pageDone) {
        // CaptionText 8c06f0a0 uses an integer budget for English/French.
        // This is not GC's 1.75-unit cursor: normal/tab characters cost two,
        // punctuation three, and a line feed costs nothing.
        s32 remaining = m_typeTicks / 2;
        usize shown = 0;
        while (remaining > 0 && shown < page.size()) {
            const char value = page[shown++];
            if (value != '\n') {
                remaining -= value == ',' || value == '.' ? 3 : 2;
            }
        }
        m_caption->shown = shown;
        m_pageDone = shown == page.size() && remaining > 0;
    }
    if (!m_pageDone) {
        return false;
    }
    // The native branch checks the previous hold before incrementing it.
    if (m_holdTicks < group.pageHoldTicks) {
        m_holdTicks += 2;
        return false;
    }
    ++m_caption->page;
    m_caption->shown = 0;
    m_typeTicks = 0;
    m_holdTicks = 0;
    m_pageDone = false;
    events.turnToNextPlayer = true;
    if (m_caption->page >= group.keys.size()) {
        m_caption.reset();
        m_groupDone = true;
    }
    return false;
}

void LegendsDreamcastBossCeremony::tick(std::span<const std::string> pages, bool speechPlaying,
                                        Events& events) {
    switch (m_stage) {
    case Stage::None:
    case Stage::Done: break;
    case Stage::Waiting:
        m_ticksLeft -= 2;
        if (m_ticksLeft <= 0) {
            m_stage = Stage::Appearing;
            events.appeared = true;
            events.turnToNextPlayer = true;
        }
        break;
    case Stage::Appearing:
        // 8c06fb54 decrements transparency by four per invocation, not four
        // times the sixtieth-tick delta. This adapter runs it on the engine's
        // fixed 30 Hz world update; the original main-loop cadence is unverified.
        m_transparency = std::max(0, m_transparency - 4);
        if (m_transparency == 0) {
            startGroup(0, events);
        }
        break;
    case Stage::Defeat:
    case Stage::Runes:
    case Stage::Quest:
        if (typePage(pages, events)) {
            const usize next = groupIndex() + 1;
            if (next < m_groups.size()) {
                startGroup(next, events);
            } else {
                m_stage = Stage::Speech;
                m_caption.reset();
            }
        }
        break;
    case Stage::Speech:
        // 8c070720 keeps resetting the exit counter while queued speech plays.
        m_ticksLeft = m_allRunes ? 240 : 120;
        if (!speechPlaying) {
            m_stage = Stage::Leaving;
            events.depart = true;
        }
        break;
    case Stage::Leaving:
        m_ticksLeft = std::max(0, m_ticksLeft - 2);
        events.sparkle = m_ticksLeft == 20;
        if (m_ticksLeft == 0) {
            m_stage = Stage::Done;
            events.finished = true;
        }
        break;
    }
}

LegendsDreamcastBossCeremony::Events
LegendsDreamcastBossCeremony::step(s32 ticks, std::span<const std::string> pages,
                                   bool speechPlaying) {
    Events result;
    if (ticks <= 0 || !running()) {
        return result;
    }
    m_pendingTicks += std::min(ticks, std::numeric_limits<s32>::max() - m_pendingTicks);
    while (m_pendingTicks >= 2 && running()) {
        const Stage previous = m_stage;
        m_pendingTicks -= 2;
        tick(pages, speechPlaying, result);
        // Caller supplies the current group's pages. Stop at a group boundary
        // so a large time chunk cannot type the previous group's text or lose
        // a voice event; remaining ticks are consumed by the next call.
        if (previous != m_stage) {
            break;
        }
    }
    return result;
}

} // namespace gdl::game
