#pragma once

#include <array>
#include <span>
#include <string>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
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
    static constexpr s32 kY = 8;
    static constexpr s32 kPieceWidth = 256;
    static constexpr s32 kMostPieces = 2;
    static constexpr f32 kEasePerTick = 3.0f;
    static constexpr u8 kAlpha = 143; ///< the original's blit alpha of 112, inverted
    static constexpr Color kFrozenTint = Color::rgba(0x80, 0x80, 0xFF, kAlpha);

    /** Lays it out for a boss of `meter` at `left`, drawing from `textures`; false when the
     * boss has no meter to show. */
    bool bind(const HealthMeterDefinition& meter, TextureSet* textures, s32 left = 0);
    void clear();
    bool bound() const { return m_pieces > 0; }

    /** Eases the health shown toward `health` of `max` over `ticks`; `alive` false takes the
     * meter down, `frozen` tints it. */
    void update(s32 ticks, f32 health, f32 maxHealth, bool alive, bool frozen);
    f32 shown() const { return m_shown; }
    bool showing() const { return bound() && m_alive; }
    /** How wide each strip's fill is drawn, in pixels of the virtual screen. */
    std::array<s32, kMostPieces> fillWidths() const;

    void draw(Canvas& canvas, RenderDevice& device) const;

private:
    TextureSet* m_textures = nullptr;
    std::string m_name;
    s32 m_pieces = 0;
    s32 m_left = 0;
    s32 m_leftInset = 0;
    s32 m_rightInset = 0;
    bool m_backed = false;
    f32 m_max = 1.0f;
    f32 m_shown = 0.0f;
    bool m_fresh = true; ///< nothing shown yet: the first health is taken as it is
    bool m_alive = true;
    bool m_frozen = false;
};

/** Ordered boss HUD layers: unbacked head fills share the preceding background's origin. */
class BossMeters {
public:
    static constexpr usize kCapacity = 3;
    void bind(std::span<const HealthMeterReading> readings, TextureSet* textures);
    void update(s32 ticks, std::span<const HealthMeterReading> readings, bool alive, bool frozen);
    void clear();
    bool bound() const { return m_count > 0; }
    bool showing() const { return bound() && m_meters[0].showing(); }
    usize count() const { return m_count; }
    const BossMeter& meter(usize index) const { return m_meters.at(index); }
    void draw(Canvas& canvas, RenderDevice& device) const;

private:
    std::array<BossMeter, kCapacity> m_meters;
    usize m_count = 0;
};

} // namespace gdl::game
