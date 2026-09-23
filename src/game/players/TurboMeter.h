#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** How the meter looks this tick, for whoever draws it. */
struct TurboMeterLook {
    f32 fill = 0.0f; ///< how much of the bar's width the front colour covers, from its middle
    Color front = Color::black();
    Color back = Color::black(); ///< the whole bar, behind it
    u8 glow = 0;                 ///< the full meter's glow, pulsing
    s32 gleam = -1;              ///< the frame of the gleam that marks a change of zone, or none
};

/**
 * A character's turbo meter, as the original keeps it: what it holds climbs by itself to a
 * hundred while the character is free to act, what is shown chases that (a point a tick up,
 * two down), and it reads in three zones: under two fifths yellow on black, then red on
 * yellow, and full, when it is red and glows. Crossing between zones gleams. Turbo moves
 * spend it; it starts each level, and ends each life, empty.
 */
class TurboMeter {
public:
    static constexpr f32 kFull = 100.0f;
    static constexpr f32 kStrongCost = 40.0f; ///< the lesser turbo attack, and what it needs
    static constexpr f32 kFullCost = 100.0f;  ///< the greater, which needs it full
    static constexpr f32 kShoveFrom = 5.0f;   ///< what a shove needs
    static constexpr f32 kFillPerSecond = 2.0f;
    static constexpr f32 kPerExperience = 0.025f; ///< what a point of experience won adds
    static constexpr f32 kShoveDrainPerSecond = 20.0f;
    static constexpr f32 kRisePerTick = 1.0f;
    static constexpr f32 kFallPerTick = 2.0f;
    static constexpr f32 kHighFrom = 0.4f; ///< of full, where the second zone begins
    static constexpr f32 kFullFrom = 0.99f;
    static constexpr s32 kGleamFrames = 5; ///< played up and back down
    static constexpr s32 kGleamTicksPerFrame = 4;
    static constexpr s32 kGlowTicks = 120; ///< one pulse of the full meter's glow

    enum class Zone : u8 { Low = 1, High = 2, Full = 3 };
    enum class Flash : u8 { None, Gleam, Glow };

    void reset();
    /** Climbs by itself for `seconds`; true the moment it comes full. */
    bool fill(f32 seconds);
    /** Runs down as a shove does. */
    void drain(f32 seconds);
    void add(f32 amount);
    /** Takes `cost` from it when it holds that much; whether it did. */
    bool spend(f32 cost);
    /** Moves what is shown towards what is held and runs the gleam and the glow. */
    void step(s32 ticks);

    f32 held() const { return m_held; }
    f32 shown() const { return m_shown; }
    Zone zone() const { return zoneOf(m_shown); }
    Flash flash() const { return m_flash; }
    static Zone zoneOf(f32 amount);
    /** How far through its zone `amount` is, nought to one. */
    static f32 zoneFraction(f32 amount);

    TurboMeterLook look() const;

private:
    f32 m_held = 0.0f;
    f32 m_shown = 0.0f;
    Flash m_flash = Flash::None;
    s32 m_flashTicks = 0;
};

} // namespace gdl::game
