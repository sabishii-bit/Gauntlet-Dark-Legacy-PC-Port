#pragma once

#include <array>
#include <cstdint>

#include "engine/assets/TextureSet.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "game/enemies/CritterData.h"

namespace gdl::game {

/**
 * A boss's health meter across the top of the screen, as the original lays it out: strips
 * of 256 (two, across the whole virtual width) from the boss's own archive, each a
 * background (`METER_BG1`, `METER_BG2`) under a fill (`METER_FG1`, `METER_FG2`) cropped to
 * the health shown, which eases toward the boss's at three a tick. The first strip's cap
 * and the last strip's tail are never fill, so a full first strip covers half the health.
 * The backgrounds turn blue while the boss stands frozen, and the meter goes with its death.
 */
class BossMeter {
public:
    static constexpr int kY = 8;
    static constexpr int kPieceWidth = 256;
    static constexpr int kMostPieces = 2;
    static constexpr float kEasePerTick = 3.0f;
    static constexpr std::uint8_t kAlpha = 143; ///< the original's blit alpha of 112, inverted
    static constexpr Color kFrozenTint = Color::rgba(0x80, 0x80, 0xFF, kAlpha);

    /** Lays it out for a boss of `meter` at `left`, drawing from `textures`; false when the
     * boss has no meter to show. */
    bool bind(const CritterMeter& meter, TextureSet* textures, int left = 0);
    void clear();
    bool bound() const { return m_pieces > 0; }

    /** Eases the health shown toward `health` of `max` over `ticks`; `alive` false takes the
     * meter down, `frozen` tints it. */
    void update(int ticks, float health, float maxHealth, bool alive, bool frozen);
    float shown() const { return m_shown; }
    bool showing() const { return bound() && m_alive; }
    /** How wide each strip's fill is drawn, in pixels of the virtual screen. */
    std::array<int, kMostPieces> fillWidths() const;

    void draw(Canvas& canvas, RenderDevice& device) const;

private:
    TextureSet* m_textures = nullptr;
    int m_pieces = 0;
    int m_left = 0;
    int m_leftInset = 0;
    int m_rightInset = 0;
    bool m_backed = false;
    float m_max = 1.0f;
    float m_shown = 0.0f;
    bool m_fresh = true; ///< nothing shown yet: the first health is taken as it is
    bool m_alive = true;
    bool m_frozen = false;
};

} // namespace gdl::game
