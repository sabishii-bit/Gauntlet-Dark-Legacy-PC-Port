#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

/** How often a help message comes up. */
enum class HelpRepeat : std::uint8_t {
    OnceForAll,     ///< until every character playing has ever seen it
    OncePerPlayer,  ///< until the character it is for has ever seen it
    OncePerSession, ///< until any character playing has seen it since it was loaded
    Always          ///< every time: news, not a lesson
};

/** One of the original's help messages: its text in the game's strings, the line spoken for
 * it, how often it comes, and how it stands against one already up. */
struct HelpMessageSpec {
    int id = 0;
    std::string_view text;  ///< a message of the game's strings
    std::string_view voice; ///< from the narrator's bank, or the character's own class's
    HelpRepeat repeat = HelpRepeat::OnceForAll;
    int line = -1;     ///< the one line of the text that is shown; all of them when -1
    int priority = 50; ///< one higher than what is up takes its place
    bool classVoice = false;
};

/** Someone a message can be for: the player, and the messages their character has seen. */
struct HelpReader {
    int player = 0;
    std::vector<int>* seen = nullptr;  ///< ever; kept sorted; saved with the character
    std::vector<int>* heard = nullptr; ///< since the character was loaded; kept sorted
};

/**
 * The help messages the original teaches the game with: a small scroll over a character's
 * head with a line or two and the narrator saying it, each shown until every character in
 * play has seen it (or, for a few, until that character has), one at a time, for a second a
 * line and half a second more, and after each a growing pause before the next. The names of
 * the classes' turbo attacks are among them, announced in the class's own bank once each
 * session as the move comes out, over whatever lesser message is up.
 */
class HelpMessages {
public:
    static constexpr int kDoorNeedsKey = 1;
    static constexpr int kChestNeedsKey = 2;
    static constexpr int kKeysFull = 4;
    static constexpr int kNoPotion = 6;
    static constexpr int kTrapsHurt = 21;
    static constexpr int kRandomChest = 23;
    static constexpr int kBarrelsHold = 27;
    static constexpr int kFirstTurboName = 57; ///< three to a class: none, the lesser, the greater
    static constexpr int kLastTurboName = 79;
    static constexpr int kAlreadyHaveRune = 90;
    static constexpr int kUseTurbo = 110;
    /** A legend item's name is this plus the realm it is for, one to eleven. */
    static constexpr int kFirstLegendName = 113;
    static constexpr int kLastLegendName = 124;
    static constexpr int kHealthFull = 133;
    static constexpr int kBlastsDestroy = 135;
    static constexpr int kGasSpoils = 136;
    static constexpr int kChestsExplode = 137;
    static constexpr int kLevelUp = 34; ///< "LEVEL %d", the number filled in

    static constexpr int kTicksPerLine = 60;
    static constexpr int kTicksOver = 30;
    static constexpr std::array<int, 5> kPauses{0, 120, 240, 420, 600}; ///< ticks, each longer
    static constexpr int kAboveHead = 62;    ///< how far over the character it is centred
    static constexpr int kMarginAcross = 64; ///< the scroll is this much wider than its text
    static constexpr int kMarginDown = 16;
    static constexpr int kLowest = 304; ///< the scroll stays over the status boxes
    static constexpr int kWidest = 511;
    static constexpr std::uint8_t kScrollAlpha = 128;

    static const HelpMessageSpec* specOf(int id);
    /** The ink a player's messages are written in. */
    static Color inkOf(int player);

    void setTexts(const MessageTable* strings) { m_strings = strings; }
    void clear();

    /** Posts message `id` for `player` unless it has been seen, another is up, or the pause
     * since the last is not over; marks it seen. The spec when it went up, for its voice. */
    const HelpMessageSpec* post(int id, int player, std::span<const HelpReader> party,
                                int number = -1);
    void update(int ticks);

    bool showing() const { return m_ticksLeft > 0; }
    int player() const { return m_player; }
    int id() const { return m_id; }
    const std::vector<std::string>& lines() const { return m_lines; }

    /** Draws it centred over `head`, a point of the virtual screen, kept on the screen. */
    void draw(Canvas& canvas, const TextPainter& text, const Texture* scroll,
              const Vec2& head) const;
    /** Where its scroll lies for a character whose head is at `head`. */
    Rect areaFor(const TextPainter& text, const Vec2& head) const;

private:
    const MessageTable* m_strings = nullptr;
    std::vector<std::string> m_lines;
    int m_id = -1;
    int m_priority = 0;
    int m_player = 0;
    int m_ticksLeft = 0;
    int m_pauseLeft = 0;
    std::size_t m_posted = 0;
};

} // namespace gdl::game
