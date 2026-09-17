#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/menu/MenuInput.h"

namespace gdl::game {

/**
 * The arcade-style name picker: up and down cycle the pending letter (and keep cycling,
 * faster and faster, while held), right or Select enters it, left removes the last one, and
 * the end mark finishes. Typed characters go straight in and Backspace erases, for players
 * with a keyboard. A finished name then flashes for a moment before the entry reports done.
 */
class NameEntry {
public:
    static constexpr usize kMaxLength = 6;
    static constexpr char kEndMark = '@';
    static constexpr s32 kFlashTicks = 60;
    static constexpr s32 kFlashPeriod = 16;

    /** Ticks between letter changes while a direction stays held: the original's ladder. */
    static constexpr std::array<s32, 13> kRepeatLadder{30, 20, 10, 6, 3, 3, 3, 3, 2, 2, 2, 2, 1};

    enum class Event : u8 { None, LetterChanged, LetterAdded, LetterRemoved, Accepted };

    /** Starts editing from `existing`, keeping up to five of its letters. */
    void begin(std::string_view existing);

    /** Applies one frame; Accepted fires once when the name is taken. */
    Event update(const MenuInput& input, s32 ticks);

    bool editing() const { return m_phase == Phase::Editing; }
    bool flashing() const { return m_phase == Phase::Flashing; }
    bool finished() const { return m_phase == Phase::Finished; }

    /** Whether the flashing name is visible this frame. */
    bool flashVisible() const { return (m_timer & kFlashPeriod) != 0; }

    const std::string& name() const { return m_name; }
    usize length() const { return m_name.size(); }
    char pendingLetter() const { return m_pending; }

    /** The letter after `letter` in the picker's cycle: A-Z, _, 0-9, the end mark. */
    static char nextLetter(char letter);
    static char previousLetter(char letter);

    /** A name for players who accept an empty one. */
    static std::string_view randomName(u32 seed);

    /** The picker letter a typed character stands for: letters in either case, digits, and
     * a space or underscore as the underscore. Anything else is none. */
    static std::optional<char> typedLetter(char typed);

private:
    enum class Phase : u8 { Editing, Flashing, Finished };

    /** Cycles the pending letter by `direction` (+1 up, -1 down). */
    void cycle(s32 direction);

    /** One frame of held-direction repeats; true when a letter changed. */
    bool repeat(const MenuInput& input, s32 ticks);

    /** Drops the last letter back into the pending slot. */
    Event removeLast();

    /** Takes the name (a random one when empty) and starts the flash. */
    Event finish();

    Phase m_phase = Phase::Finished;
    std::string m_name;
    char m_pending = kEndMark;
    s32 m_timer = 0;
    s32 m_repeatDirection = 0;
    s32 m_repeatCounter = 0;
    usize m_repeatStep = 0;
};

} // namespace gdl::game
