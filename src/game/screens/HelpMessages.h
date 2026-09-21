#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

/** One of the original's help messages: its text in the game's strings, the narrator's line
 * for it, and whether each player is told for themselves or the party once together. */
struct HelpMessageSpec {
    s32 id = 0;
    std::string_view text;  ///< a message of the game's strings
    std::string_view voice; ///< from the narrator's bank
    bool perPlayer = false;
};

/** Someone a message can be for: the player, and the messages their character has seen. */
struct HelpReader {
    s32 player = 0;
    std::vector<s32>* seen = nullptr; ///< kept sorted; saved with the character
};

/**
 * The help messages the original teaches the game with: a small scroll over a character's
 * head with a line or two and the narrator saying it, each shown until every character in
 * play has seen it (or, for a few, until that character has), one at a time, for a second a
 * line and half a second more, and after each a growing pause before the next.
 */
class HelpMessages {
public:
    static constexpr s32 kDoorNeedsKey = 1;
    static constexpr s32 kChestNeedsKey = 2;
    static constexpr s32 kKeysFull = 4;
    static constexpr s32 kNoPotion = 6;
    static constexpr s32 kTrapsHurt = 21;
    static constexpr s32 kRandomChest = 23;
    static constexpr s32 kBarrelsHold = 27;
    static constexpr s32 kUseTurbo = 110;
    static constexpr s32 kHealthFull = 133;
    static constexpr s32 kBlastsDestroy = 135;
    static constexpr s32 kGasSpoils = 136;
    static constexpr s32 kChestsExplode = 137;

    static constexpr s32 kTicksPerLine = 60;
    static constexpr s32 kTicksOver = 30;
    static constexpr std::array<s32, 5> kPauses{0, 120, 240, 420, 600}; ///< ticks, each longer
    static constexpr s32 kAboveHead = 62;    ///< how far over the character it is centred
    static constexpr s32 kMarginAcross = 64; ///< the scroll is this much wider than its text
    static constexpr s32 kMarginDown = 16;
    static constexpr s32 kLowest = 304; ///< the scroll stays over the status boxes
    static constexpr s32 kWidest = 511;
    static constexpr u8 kScrollAlpha = 128;

    static const HelpMessageSpec* specOf(s32 id);
    /** The ink a player's messages are written in. */
    static Color inkOf(s32 player);

    void setTexts(const MessageTable* strings) { m_strings = strings; }
    void clear();

    /** Posts message `id` for `player` unless it has been seen, another is up, or the pause
     * since the last is not over; marks it seen. The spec when it went up, for its voice. */
    const HelpMessageSpec* post(s32 id, s32 player, std::span<const HelpReader> party);
    void update(s32 ticks);

    bool showing() const { return m_ticksLeft > 0; }
    s32 player() const { return m_player; }
    s32 id() const { return m_id; }
    const std::vector<std::string>& lines() const { return m_lines; }

    /** Draws it centred over `head`, a point of the virtual screen, kept on the screen. */
    void draw(Canvas& canvas, const TextPainter& text, const Texture* scroll,
              const Vec2& head) const;
    /** Where its scroll lies for a character whose head is at `head`. */
    Rect areaFor(const TextPainter& text, const Vec2& head) const;

private:
    const MessageTable* m_strings = nullptr;
    std::vector<std::string> m_lines;
    s32 m_id = -1;
    s32 m_player = 0;
    s32 m_ticksLeft = 0;
    s32 m_pauseLeft = 0;
    usize m_posted = 0;
};

} // namespace gdl::game
