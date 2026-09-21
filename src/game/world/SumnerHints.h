#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"

#include "game/players/Progression.h"

namespace gdl::game {

/** What Sumner can be asked about. */
enum class HintTopic : u8 { General, Guardians, Legends, Runestones };

/** What the party has done, as far as it steers Sumner's hints. Worlds go by the original's
 * world ids, runestones by their number less one. */
struct HintKnowledge {
    u32 worldsOpen = 0;
    u32 guardiansBeaten = 0;
    u32 legendsFound = 0;
    u32 runestonesFound = 0;
    std::array<bool, 3> wingsOpen{}; ///< the gargoyle gates the party has the icons for
    /** Bits of the worlds (or runestones) the party has tried once and twice: each try
     * earns a further passage of the hint. */
    std::array<u32, 2> guardianTries{};
    std::array<u32, 2> legendTries{};
    std::array<u32, 2> runestoneTries{};

    /** What a party knows from its members' progress: the worlds their crystals open. */
    static HintKnowledge ofParty(std::span<const ClassProgress> party);
};

/** One hint as the scroll shows it. */
struct HintPage {
    std::string title;
    std::vector<std::string> passages; ///< each with its own line breaks
    f32 scale = 1.0f;                  ///< of the hint's message
    bool centred = false; ///< a lone passage about the scroll's middle, else from the top
    s32 gap = 0;          ///< pixels between passages
};

/**
 * Sumner's hints, chosen the way the original chooses them: each topic steps on to its next
 * hint every time it is asked, general hints running through the list of the furthest wing
 * the party has opened, guardians and legend items through the open worlds still to be
 * beaten or found (then all the open ones, then the first), runestones through those still
 * missing. A guardian, item or runestone tried before shows a passage more each time.
 */
class SumnerHints {
public:
    static constexpr std::array<s32, 14> kWorldOrder{13, 7, 2, 1, 11, 4, 3, 9, 10, 5, 6, 8, 0, 0};
    static constexpr std::array<s32, 13> kRunestoneOrder{8, 9, 4, 1, 2, 3, 10, 7, 5, 6, 11, 12, 13};
    static constexpr s32 kGuardianCount = 11; ///< of kWorldOrder the guardian hints run over
    static constexpr s32 kLegendCount = 10;
    static constexpr s32 kPassageGap = 16; ///< between a guardian's or an item's passages

    /** Reads the unpacked hint texts; false (with a warning) when they are missing. */
    bool load(const std::filesystem::path& file);
    /** Takes the hints' words from the game's strings, under `hint.`. */
    void translate(const StringTable& strings) { m_text.translate(strings, kTextPrefix); }
    static constexpr std::string_view kTextPrefix = "hint";
    bool loaded() const { return m_text.loaded(); }

    /** Starts the guardian, legend and runestone hints over, as each visit to him does; the
     * general hints carry on from where they were. */
    void beginVisit();
    /** Steps the topic on and returns its hint; an empty page when the texts lack it. */
    HintPage next(HintTopic topic, const HintKnowledge& knowledge, std::string_view generalTitle);

    s32 generalList() const { return m_generalList; }
    s32 generalHint() const { return m_generalHint; }
    s32 guardian() const { return m_guardian.index; }
    s32 legend() const { return m_legend.index; }
    s32 runestone() const { return m_runestone.index; }

private:
    /** Where a topic has got to: the entry it shows and how far its search has widened. */
    struct Cursor {
        s32 index = 0;
        s32 widened = 0;
    };

    void stepGeneral(const HintKnowledge& knowledge);
    static void stepWorlds(Cursor& cursor, s32 count, u32 open, u32 done);
    static void stepRunestones(Cursor& cursor, u32 found);
    static s32 passagesFor(u32 bit, const std::array<u32, 2>& tries);
    HintPage pageOf(std::string_view titles, usize title, std::string_view list, usize entry,
                    s32 passages, s32 gap) const;

    MessageTable m_text;
    s32 m_generalList = -1;
    s32 m_generalHint = -1;
    Cursor m_guardian;
    Cursor m_legend;
    Cursor m_runestone{-1, 0};
};

} // namespace gdl::game
