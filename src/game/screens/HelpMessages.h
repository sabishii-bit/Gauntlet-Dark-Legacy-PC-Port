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
    std::int32_t id = 0;
    std::string_view text;  ///< a message of the game's strings
    std::string_view voice; ///< from the narrator's bank, or the character's own class's
    HelpRepeat repeat = HelpRepeat::OnceForAll;
    std::int32_t line = -1;     ///< the one line of the text that is shown; all of them when -1
    std::int32_t priority = 50; ///< one higher than what is up takes its place
    bool classVoice = false;
};

/** Someone a message can be for: the player, and the messages their character has seen. */
struct HelpReader {
    std::int32_t player = 0;
    std::vector<std::int32_t>* seen = nullptr;  ///< ever; kept sorted; saved with the character
    std::vector<std::int32_t>* heard = nullptr; ///< since the character was loaded; kept sorted
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
    static constexpr std::int32_t kDoorNeedsKey = 1;
    static constexpr std::int32_t kChestNeedsKey = 2;
    static constexpr std::int32_t kKeysFull = 4;
    static constexpr std::int32_t kNoPotion = 6;
    static constexpr std::int32_t kTrapsHurt = 21;
    static constexpr std::int32_t kRandomChest = 23;
    static constexpr std::int32_t kBarrelsHold = 27;
    static constexpr std::int32_t kFirstTurboName =
        57; ///< three to a class: none, the lesser, the greater
    static constexpr std::int32_t kLastTurboName = 79;
    static constexpr std::int32_t kAlreadyHaveRune = 90;
    static constexpr std::int32_t kUseTurbo = 110;
    /** A legend item's name is this plus the realm it is for, one to eleven. */
    static constexpr std::int32_t kFirstLegendName = 113;
    static constexpr std::int32_t kLastLegendName = 124;
    static constexpr std::int32_t kHealthFull = 133;
    static constexpr std::int32_t kBlastsDestroy = 135;
    static constexpr std::int32_t kGasSpoils = 136;
    static constexpr std::int32_t kChestsExplode = 137;
    static constexpr std::int32_t kLevelUp = 34; ///< "LEVEL %d", the number filled in

    static constexpr std::int32_t kTicksPerLine = 60;
    static constexpr std::int32_t kTicksOver = 30;
    static constexpr std::array<std::int32_t, 5> kPauses{0, 120, 240, 420,
                                                         600}; ///< ticks, each longer
    static constexpr std::int32_t kAboveHead = 62; ///< how far over the character it is centred
    static constexpr std::int32_t kMarginAcross =
        64; ///< the scroll is this much wider than its text
    static constexpr std::int32_t kMarginDown = 16;
    static constexpr std::int32_t kLowest = 304; ///< the scroll stays over the status boxes
    static constexpr std::int32_t kWidest = 511;
    static constexpr std::uint8_t kScrollAlpha = 128;

    static const HelpMessageSpec* specOf(std::int32_t id);
    /** The ink a player's messages are written in. */
    static Color inkOf(std::int32_t player);

    void setTexts(const MessageTable* strings) { m_strings = strings; }
    void clear();

    /** Posts message `id` for `player` unless it has been seen, another is up, or the pause
     * since the last is not over; marks it seen. The spec when it went up, for its voice. */
    const HelpMessageSpec* post(std::int32_t id, std::int32_t player,
                                std::span<const HelpReader> party, std::int32_t number = -1);
    void update(std::int32_t ticks);

    bool showing() const { return m_ticksLeft > 0; }
    std::int32_t player() const { return m_player; }
    std::int32_t id() const { return m_id; }
    const std::vector<std::string>& lines() const { return m_lines; }

    /** Draws it centred over `head`, a point of the virtual screen, kept on the screen. */
    void draw(Canvas& canvas, const TextPainter& text, const Texture* scroll,
              const Vec2& head) const;
    /** Where its scroll lies for a character whose head is at `head`. */
    Rect areaFor(const TextPainter& text, const Vec2& head) const;

private:
    const MessageTable* m_strings = nullptr;
    std::vector<std::string> m_lines;
    std::int32_t m_id = -1;
    std::int32_t m_priority = 0;
    std::int32_t m_player = 0;
    std::int32_t m_ticksLeft = 0;
    std::int32_t m_pauseLeft = 0;
    std::size_t m_posted = 0;
};

} // namespace gdl::game
