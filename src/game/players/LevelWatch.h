#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace gdl::game {

/** A level gained (or lost): from what, to what, and whether it crossed a tier. */
struct LevelChange {
    std::int32_t player = -1;
    std::int32_t from = 1;
    std::int32_t to = 1;
    /** Whether the change crossed a milestone: every tenth level, when the costume changes
     * and the class has something to say. */
    bool milestone() const { return to / kLevelsPerTier != from / kLevelsPerTier; }
    bool gained() const { return to > from; }

    static constexpr std::int32_t kLevelsPerTier = 10;
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
    std::optional<LevelChange> observe(std::int32_t player, std::int32_t level);
    /** Forgets a player, or everyone. */
    void forget(std::int32_t player);
    void clear() { m_marks.clear(); }
    std::optional<std::int32_t> markOf(std::int32_t player) const;

private:
    struct Mark {
        std::int32_t player = -1;
        std::int32_t level = 1;
    };
    std::vector<Mark> m_marks;
};

} // namespace gdl::game
