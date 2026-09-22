#pragma once

#include <optional>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** A level gained (or lost): from what, to what, and whether it crossed a tier. */
struct LevelChange {
    s32 player = -1;
    s32 from = 1;
    s32 to = 1;
    /** Whether the change crossed a milestone: every tenth level, when the costume changes
     * and the class has something to say. */
    bool milestone() const { return to / kLevelsPerTier != from / kLevelsPerTier; }
    bool gained() const { return to > from; }

    static constexpr s32 kLevelsPerTier = 10;
};

/**
 * Watches the party's levels for changes. Told each player's level as often as it likes, it
 * reports the changes since it last looked, so whatever grants experience need not know what
 * a level means, and whatever answers a level gained (the fanfare, the costume) need not
 * know where the experience came from.
 */
class LevelWatch {
public:
    /** Notes `level` for `player`; the first sighting sets the mark and reports nothing. */
    std::optional<LevelChange> observe(s32 player, s32 level);
    /** Forgets a player, or everyone. */
    void forget(s32 player);
    void clear() { m_marks.clear(); }
    std::optional<s32> markOf(s32 player) const;

private:
    struct Mark {
        s32 player = -1;
        s32 level = 1;
    };
    std::vector<Mark> m_marks;
};

} // namespace gdl::game
