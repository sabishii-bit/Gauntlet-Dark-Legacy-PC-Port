#include "game/players/HeadGem.h"

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"

namespace gdl::game {

bool HeadGem::update(u32 special) {
    if ((special & powerup::kHandOfDeath) != 0) {
        if (m_handOfDeath) {
            return false;
        }
        m_handOfDeath = true;
        m_shown = kHandOfDeath;
        return true;
    }
    if ((special & powerup::kHealthVamp) != 0) {
        if (m_healthVamp) {
            return false;
        }
        m_healthVamp = true;
        m_shown = kHealthVamp;
        return true;
    }
    m_handOfDeath = false;
    m_healthVamp = false;
    m_shown = {};
    return false;
}

} // namespace gdl::game
