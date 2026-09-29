#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The gem a hand of death or a health vampire sets on the wearer's head (PlayerProcessPowerups,
 * player.c 5625): one at a time, the hand of death first, each greeted once with a gem burst
 * as it appears, and forgotten only once neither is worn.
 */
class HeadGem {
public:
    /** HEAD_HANDOFDEATH: the archive keeps fifteen letters of an object's name. */
    static constexpr std::string_view kHandOfDeath = "HEAD_HANDOFDEAT";
    static constexpr std::string_view kHealthVamp = "HEAD_HEALTHVAMP";
    /** StartGemFX(pos, 1): effect 70 of the table, a POWERUPS tree. */
    static constexpr std::string_view kAppearEffect = "GETGEMORANGE";

    /** Follows the specials worn this frame; whether a gem appeared (its burst plays). */
    bool update(u32 special);
    /** The gem object on the head, none without one. It changes only as a gem appears. */
    std::string_view shown() const { return m_shown; }

private:
    bool m_handOfDeath = false; ///< field_A1E
    bool m_healthVamp = false;  ///< field_A20
    std::string_view m_shown;   ///< gem_object
};

} // namespace gdl::game
