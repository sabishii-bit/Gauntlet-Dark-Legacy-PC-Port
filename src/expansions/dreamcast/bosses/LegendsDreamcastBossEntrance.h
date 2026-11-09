#pragma once

#include <random>
#include <span>
#include <vector>

#include "formats/WorldFile.h"
#include "game/level/FallingPiece.h"

namespace gdl::game {

/** Dreamcast E2's item-based entrance masonry. Its three impulses come from
 * Skorne's authored SFXX arena cues, not a guessed timer after loading. */
class LegendsDreamcastBossEntrance {
public:
    struct Piece {
        u32 nativeIndex = 0;
        FallingPiece motion;
    };
    static bool accepts(const formats::ItemInfoRecord& info,
                        const formats::ItemInstanceRecord& instance);
    void clear(f32 bottom);
    void add(u32 index, Vec3 position, Vec3 rotation);
    void cue(Vec3 boss);
    void update(f32 seconds);
    std::span<const Piece> pieces() const { return m_pieces; }
    s32 phase() const { return m_phase; }
    f32 presentationAlpha(f32 alpha) const;

private:
    std::vector<Piece> m_pieces;
    std::mt19937 m_random{0};
    f32 m_bottom = -200;
    f32 m_remainder = 0;
    f32 m_updateSeconds = 0;
    s32 m_phase = 0;
};

} // namespace gdl::game
