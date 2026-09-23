#pragma once

#include <optional>
#include <vector>

namespace gdl::game {

/** A level gained (or lost): from what, to what, and whether it crossed a tier. */
struct LevelChange {
    int player = -1;
    int from = 1;
    int to = 1;
    /** Whether the change crossed a milestone: every tenth level, when the costume changes
     * and the class has something to say. */
    bool milestone() const { return to / kLevelsPerTier != from / kLevelsPerTier; }
    bool gained() const { return to > from; }

    static constexpr int kLevelsPerTier = 10;
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
    std::optional<LevelChange> observe(int player, int level);
    /** Forgets a player, or everyone. */
    void forget(int player);
    void clear() { m_marks.clear(); }
    std::optional<int> markOf(int player) const;

private:
    struct Mark {
        int player = -1;
        int level = 1;
    };
    std::vector<Mark> m_marks;
};

} // namespace gdl::game
