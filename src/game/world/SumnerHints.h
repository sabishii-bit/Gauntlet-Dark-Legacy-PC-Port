#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/MessageTable.h"

#include "game/players/Progression.h"

namespace gdl::game {

/** What Sumner can be asked about. */
enum class HintTopic : std::uint8_t { General, Guardians, Legends, Runestones };

/** What the party has done, as far as it steers Sumner's hints. Worlds go by the original's
 * world ids, runestones by their number less one. */
struct HintKnowledge {
    std::uint32_t worldsOpen = 0;
    std::uint32_t guardiansBeaten = 0;
    std::uint32_t legendsFound = 0;
    std::uint32_t runestonesFound = 0;
    std::array<bool, 3> wingsOpen{}; ///< the gargoyle gates the party has the icons for
    /** Bits of the worlds (or runestones) the party has tried once and twice: each try
     * earns a further passage of the hint. */
    std::array<std::uint32_t, 2> guardianTries{};
    std::array<std::uint32_t, 2> legendTries{};
    std::array<std::uint32_t, 2> runestoneTries{};

    /** What a party knows from its members' progress: the worlds their crystals open. */
    static HintKnowledge ofParty(std::span<const ClassProgress> party);
};

/** One hint as the scroll shows it. */
struct HintPage {
    std::string title;
    std::vector<std::string> passages; ///< each with its own line breaks
    float scale = 1.0f;                ///< of the hint's message
    bool centred = false; ///< a lone passage about the scroll's middle, else from the top
    std::int32_t gap = 0; ///< pixels between passages
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
    static constexpr std::array<std::int32_t, 14> kWorldOrder{13, 7,  2, 1, 11, 4, 3,
                                                              9,  10, 5, 6, 8,  0, 0};
    static constexpr std::array<std::int32_t, 13> kRunestoneOrder{8, 9, 4, 1,  2,  3, 10,
                                                                  7, 5, 6, 11, 12, 13};
    static constexpr std::int32_t kGuardianCount =
        11; ///< of kWorldOrder the guardian hints run over
    static constexpr std::int32_t kLegendCount = 10;
    static constexpr std::int32_t kPassageGap = 16; ///< between a guardian's or an item's passages

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

    std::int32_t generalList() const { return m_generalList; }
    std::int32_t generalHint() const { return m_generalHint; }
    std::int32_t guardian() const { return m_guardian.index; }
    std::int32_t legend() const { return m_legend.index; }
    std::int32_t runestone() const { return m_runestone.index; }

private:
    /** Where a topic has got to: the entry it shows and how far its search has widened. */
    struct Cursor {
        std::int32_t index = 0;
        std::int32_t widened = 0;
    };

    void stepGeneral(const HintKnowledge& knowledge);
    static void stepWorlds(Cursor& cursor, std::int32_t count, std::uint32_t open,
                           std::uint32_t done);
    static void stepRunestones(Cursor& cursor, std::uint32_t found);
    static std::int32_t passagesFor(std::uint32_t bit, const std::array<std::uint32_t, 2>& tries);
    HintPage pageOf(std::string_view titles, std::size_t title, std::string_view list,
                    std::size_t entry, std::int32_t passages, std::int32_t gap) const;

    MessageTable m_text;
    std::int32_t m_generalList = -1;
    std::int32_t m_generalHint = -1;
    Cursor m_guardian;
    Cursor m_legend;
    Cursor m_runestone{-1, 0};
};

} // namespace gdl::game
