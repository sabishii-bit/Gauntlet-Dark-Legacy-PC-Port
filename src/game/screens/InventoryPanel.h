#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/Progression.h"
#include "game/players/Relics.h"

namespace gdl::game {

/** What a character's inventory panel shows, taken from it as the panel opens. */
struct InventoryContents {
    static constexpr usize kProvinceCount = 8;
    std::array<s32, Relics::kGargoyleKinds> gargoylePieces{};
    /** Gathered towards the eight provinces' gates: realms 1 to 8, the tower's crystal colours. */
    std::array<s32, kProvinceCount> crystals{};
    u16 legends = 0; ///< the legend items held, a bit per realm
    u16 runes = 0;   ///< the runestones held, a bit each

    static InventoryContents of(const ClassProgress& progress);
    bool operator==(const InventoryContents&) const = default;
};

/** One SELECT picture of the panel where its animation has it this tick. */
struct InventoryPiece {
    std::string_view texture;
    s32 x = 0; ///< left edge in virtual pixels, the lane's offset included
    s32 y = 0;
    f32 size = 1.0f; ///< times the picture's own width and height
    u8 opacity = 255;
};

/** A count written beside a piece: `n` ends at x, `/m` starts there. */
struct InventoryCount {
    s32 x = 0;
    s32 y = 0;
    f32 scale = 1.0f;
    s32 n = 0;
    s32 m = 0;
    u8 opacity = 255;
};

/**
 * One player's column of the between-level inventory panel (draw_inventory_panel): the
 * SELECT window with the gargoyle pieces and crystals counted against what the tower wants,
 * the legend items held (their `_EMPTY` pictures otherwise) and the boss glass pieces. It
 * flies in over 120 ticks, the pieces spiralling in from sixty pixels out at twice their
 * size, and on accept flies out over 15 ticks, pushed away from (64, 180) and growing
 * fivefold; the counts fade with it. Pure logic on a 60 Hz tick: a painter draws the views.
 */
class InventoryPanel {
public:
    enum class Phase : u8 { Closed, Entering, Idle, Leaving, Done };
    static constexpr s32 kEnterTicks = 120;
    static constexpr s32 kLeaveTicks = 15;
    static constexpr s32 kLaneWidth = 128;
    /** The accept prompt: BUTTON_X at (16, 280), 16 square, its label from 40. */
    static constexpr s32 kPromptX = 16;
    static constexpr s32 kPromptY = 280;
    static constexpr s32 kPromptSize = 16;
    static constexpr s32 kPromptLabelX = 40;
    static constexpr f32 kPromptLabelScale = 0.5f;

    void open(const InventoryContents& contents);
    /** Advances `ticks`; a fresh accept while entering or idle starts the leave (true). */
    bool step(s32 ticks, bool accept);
    Phase phase() const { return m_phase; }
    bool done() const { return m_phase == Phase::Done; }
    /** The prompt shows until the panel leaves. */
    bool showsPrompt() const { return m_phase == Phase::Entering || m_phase == Phase::Idle; }
    const InventoryContents& contents() const { return m_contents; }

    /** The pictures for a lane whose left edge is `laneX`, in drawing order. */
    std::vector<InventoryPiece> pieces(s32 laneX) const;
    std::vector<InventoryCount> counts(s32 laneX) const;

private:
    struct Placement {
        s32 x = 0;
        s32 y = 0;
        f32 size = 1.0f;
        u8 opacity = 255;
    };
    f32 progress() const;
    Placement place(s32 x, s32 y, s32 laneX) const;
    u8 textOpacity() const;

    InventoryContents m_contents;
    Phase m_phase = Phase::Closed;
    s32 m_ticks = 0;
};

} // namespace gdl::game
