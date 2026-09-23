#include "game/world/SumnerHints.h"

#include <algorithm>
#include <cstddef>

#include "engine/core/Types.h"

#include "game/world/LevelTriggers.h"

namespace gdl::game {

namespace {

constexpr std::string_view kGeneralList = "GENERAL_HINTS";
constexpr std::string_view kGuardianList = "BOSS_HINTS";
constexpr std::string_view kLegendList = "LEGEND_HINTS";
constexpr std::string_view kRunestoneList = "RUNE_HINTS";
constexpr std::string_view kGuardianTitles = "BOSSHINTDESC";
constexpr std::string_view kLegendTitles = "LEGENDHINTDESCS";
constexpr std::string_view kRunestoneTitles = "RUNEHINTDESCS";
constexpr s32 kLastGeneralList = 3;
constexpr s32 kTowerWorld = 13;
/** The realm whose crystals open each world; worlds 5, 6 and 8 want runestones instead. */
constexpr std::array<s32, 14> kWorldCrystals{0, 3, 2, 6, 5, 0, 0, 1, 0, 7, 8, 4, 0, 0};
constexpr std::array<s32, 3> kRunestoneWorlds{5, 6, 8};

u32 bitOf(s32 index) {
    return 1U << static_cast<u32>(index);
}

} // namespace

HintKnowledge HintKnowledge::ofParty(std::span<const ClassProgress> party) {
    HintKnowledge knowledge;
    for (s32 world = 0; world < static_cast<s32>(kWorldCrystals.size()); ++world) {
        if (std::ranges::find(kRunestoneWorlds, world) != kRunestoneWorlds.end()) {
            continue; // runestones are not gathered yet
        }
        const s32 realm = kWorldCrystals[static_cast<usize>(world)];
        s32 best = 0;
        for (const ClassProgress& progress : party) {
            best = std::max(best, progress.crystals[static_cast<usize>(realm)]);
        }
        if (world == kTowerWorld ||
            (!party.empty() && best >= LevelTriggers::crystalsNeeded(realm))) {
            knowledge.worldsOpen |= bitOf(world);
        }
    }
    return knowledge;
}

bool SumnerHints::load(const std::filesystem::path& file) {
    m_generalList = -1;
    m_generalHint = -1;
    beginVisit();
    return m_text.load(file);
}

void SumnerHints::beginVisit() {
    m_guardian = Cursor{0, 0};
    m_legend = Cursor{0, 0};
    m_runestone = Cursor{-1, 0};
}

/** The next hint of the list for the furthest wing open, then of the wings before it, and
 * round again. */
void SumnerHints::stepGeneral(const HintKnowledge& knowledge) {
    const MessageList* lists = m_text.findList(kGeneralList);
    for (s32 tries = 0; tries < 2 * (kLastGeneralList + 2); ++tries) {
        if (m_generalList < 0) {
            m_generalList = kLastGeneralList;
        }
        while (m_generalList > 0 && !knowledge.wingsOpen[static_cast<usize>(m_generalList - 1)]) {
            m_generalHint = -1;
            --m_generalList;
        }
        ++m_generalHint;
        const MessageInfo* message =
            lists != nullptr ? m_text.listed(*lists, static_cast<usize>(m_generalList)) : nullptr;
        const auto count = message != nullptr ? static_cast<s32>(message->pages.size()) : 0;
        if (m_generalHint < count) {
            return;
        }
        m_generalHint = -1;
        --m_generalList;
    }
}

/** The next open world not yet `done`; with none left, the next open one at all; with none
 * open, the first. */
void SumnerHints::stepWorlds(Cursor& cursor, s32 count, u32 open, u32 done) {
    s32 from = cursor.index + 1;
    for (;;) {
        if (from >= count) {
            from %= count;
            cursor.widened = 1;
        }
        from = std::max(from, 1);
        s32 at = from;
        for (; at < count; ++at) {
            const u32 world = bitOf(kWorldOrder[static_cast<usize>(at)]);
            if ((open & world) != 0 && (cursor.widened != 0 || (done & world) == 0)) {
                break;
            }
        }
        if (at != count) {
            cursor.index = at;
            return;
        }
        if (cursor.widened > 1) {
            cursor.widened = 1;
            cursor.index = 1;
            return;
        }
        ++cursor.widened;
        from = 1;
    }
}

/** The next runestone still missing; with them all found, the next one at all. */
void SumnerHints::stepRunestones(Cursor& cursor, u32 found) {
    const auto count = static_cast<s32>(kRunestoneOrder.size());
    s32 from = cursor.index + 1;
    for (;;) {
        if (from >= count) {
            from %= count;
            cursor.widened = 1;
        }
        s32 at = from;
        for (; at < count; ++at) {
            const u32 stone = bitOf(kRunestoneOrder[static_cast<usize>(at)] - 1);
            if (cursor.widened != 0 || (found & stone) == 0) {
                break;
            }
        }
        if (at != count) {
            cursor.index = at;
            return;
        }
        if (cursor.widened > 1) {
            cursor.widened = 1;
            cursor.index = 0;
            return;
        }
        ++cursor.widened;
        from = 1;
    }
}

/** One passage, and one more for each time the party has tried. */
s32 SumnerHints::passagesFor(u32 bit, const std::array<u32, 2>& tries) {
    if ((tries[1] & bit) != 0) {
        return 3;
    }
    return (tries[0] & bit) != 0 ? 2 : 1;
}

HintPage SumnerHints::pageOf(std::string_view titles, usize title, std::string_view list,
                             usize entry, s32 passages, s32 gap) const {
    HintPage page;
    page.gap = gap;
    if (const auto found = m_text.find(titles); found.has_value()) {
        const MessageInfo& names = m_text.message(*found);
        if (title < names.pages.size()) {
            page.title = names.pages[title];
        }
    }
    const MessageList* entries = m_text.findList(list);
    const MessageInfo* message = entries != nullptr ? m_text.listed(*entries, entry) : nullptr;
    if (message != nullptr) {
        page.scale = message->scale;
        const usize count = std::min(static_cast<usize>(passages), message->pages.size());
        page.passages.assign(message->pages.begin(),
                             message->pages.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return page;
}

HintPage SumnerHints::next(HintTopic topic, const HintKnowledge& knowledge,
                           std::string_view generalTitle) {
    if (!loaded()) {
        return {};
    }
    switch (topic) {
    case HintTopic::General: {
        stepGeneral(knowledge);
        HintPage page;
        page.title = std::string(generalTitle);
        page.centred = true;
        const MessageList* lists = m_text.findList(kGeneralList);
        const MessageInfo* message = lists != nullptr && m_generalList >= 0
                                         ? m_text.listed(*lists, static_cast<usize>(m_generalList))
                                         : nullptr;
        if (message != nullptr && m_generalHint >= 0 &&
            static_cast<usize>(m_generalHint) < message->pages.size()) {
            page.scale = message->scale;
            page.passages.push_back(message->pages[static_cast<usize>(m_generalHint)]);
        }
        return page;
    }
    case HintTopic::Guardians: {
        stepWorlds(m_guardian, kGuardianCount, knowledge.worldsOpen, knowledge.guardiansBeaten);
        const auto entry = static_cast<usize>(m_guardian.index);
        return pageOf(kGuardianTitles, entry, kGuardianList, entry,
                      passagesFor(bitOf(kWorldOrder[entry]), knowledge.guardianTries), kPassageGap);
    }
    case HintTopic::Legends: {
        stepWorlds(m_legend, kLegendCount, knowledge.worldsOpen, knowledge.legendsFound);
        const auto entry = static_cast<usize>(m_legend.index);
        return pageOf(kLegendTitles, entry, kLegendList, entry,
                      passagesFor(bitOf(kWorldOrder[entry]), knowledge.legendTries), kPassageGap);
    }
    case HintTopic::Runestones: {
        stepRunestones(m_runestone, knowledge.runestonesFound);
        const auto stone =
            static_cast<usize>(kRunestoneOrder[static_cast<usize>(m_runestone.index)] - 1);
        return pageOf(kRunestoneTitles, stone, kRunestoneList, stone,
                      passagesFor(bitOf(static_cast<s32>(stone)), knowledge.runestoneTries), 0);
    }
    }
    return {};
}

} // namespace gdl::game
