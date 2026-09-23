#pragma once

#include <cstdint>

#include "engine/math/Math.h"

namespace gdl::game {

/** How the meter looks this tick, for whoever draws it. */
struct TurboMeterLook {
    float fill = 0.0f; ///< how much of the bar's width the front colour covers, from its middle
    Color front = Color::black();
    Color back = Color::black(); ///< the whole bar, behind it
    std::uint8_t glow = 0;       ///< the full meter's glow, pulsing
    std::int32_t gleam = -1;     ///< the frame of the gleam that marks a change of zone, or none
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
    static constexpr float kFull = 100.0f;
    static constexpr float kStrongCost = 40.0f; ///< the lesser turbo attack, and what it needs
    static constexpr float kFullCost = 100.0f;  ///< the greater, which needs it full
    static constexpr float kShoveFrom = 5.0f;   ///< what a shove needs
    static constexpr float kFillPerSecond = 2.0f;
    static constexpr float kPerExperience = 0.025f; ///< what a point of experience won adds
    static constexpr float kShoveDrainPerSecond = 20.0f;
    static constexpr float kRisePerTick = 1.0f;
    static constexpr float kFallPerTick = 2.0f;
    static constexpr float kHighFrom = 0.4f; ///< of full, where the second zone begins
    static constexpr float kFullFrom = 0.99f;
    static constexpr std::int32_t kGleamFrames = 5; ///< played up and back down
    static constexpr std::int32_t kGleamTicksPerFrame = 4;
    static constexpr std::int32_t kGlowTicks = 120; ///< one pulse of the full meter's glow

    enum class Zone : std::uint8_t { Low = 1, High = 2, Full = 3 };
    enum class Flash : std::uint8_t { None, Gleam, Glow };

    void reset();
    /** Climbs by itself for `seconds`; true the moment it comes full. */
    bool fill(float seconds);
    /** Runs down as a shove does. */
    void drain(float seconds);
    void add(float amount);
    /** Takes `cost` from it when it holds that much; whether it did. */
    bool spend(float cost);
    /** Moves what is shown towards what is held and runs the gleam and the glow. */
    void step(std::int32_t ticks);

    float held() const { return m_held; }
    float shown() const { return m_shown; }
    Zone zone() const { return zoneOf(m_shown); }
    Flash flash() const { return m_flash; }
    static Zone zoneOf(float amount);
    /** How far through its zone `amount` is, nought to one. */
    static float zoneFraction(float amount);

    TurboMeterLook look() const;

private:
    float m_held = 0.0f;
    float m_shown = 0.0f;
    Flash m_flash = Flash::None;
    std::int32_t m_flashTicks = 0;
};

} // namespace gdl::game
