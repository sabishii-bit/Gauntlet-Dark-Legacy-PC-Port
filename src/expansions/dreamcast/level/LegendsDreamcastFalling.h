#pragma once

#include <span>
#include <vector>

#include "expansions/shared/level/LegendsPortalVisitor.h"
#include "formats/WorldFile.h"
#include "game/level/FallingPiece.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** Native contact-collapse rubble. Unlike keyed world platforms, these items
 * start at their authored position and acquire downward velocity on touch. */
class LegendsDreamcastFalling {
public:
    struct Piece {
        usize figure = 0;
        u32 nativeIndex = 0;
        s32 subtype = 40;
        FallingPiece body;
        Obstacle contact;
        bool active = false;
    };

    void clear(f32 worldMinY);
    bool add(const formats::ItemInfoRecord& info, const formats::ItemInstanceRecord& item,
             u32 nativeIndex, usize figure);
    /** Newly activated world positions, for the native collapse sound. */
    std::vector<Vec3> touch(std::span<const LegendsPortalVisitor> visitors);
    void update(f32 seconds);
    std::span<const Piece> pieces() const { return m_pieces; }
    bool visible(usize figure) const;

private:
    std::vector<Piece> m_pieces;
    f32 m_bottom = -FallingPiece::kDiscardDepth;
    f32 m_remainder = 0;
};

} // namespace gdl::game
