#include "game/effects/HeadGem.h"

#include "engine/core/Types.h"

#include "game/combat/PowerupFlags.h"

namespace gdl::game {

bool HeadGem::Checkpoint::valid() const {
    switch (shown) {
    case Shown::None: return !handOfDeath && !healthVamp;
    case Shown::HandOfDeath: return handOfDeath;
    case Shown::HealthVamp: return healthVamp;
    }
    return false;
}
HeadGem::Checkpoint HeadGem::checkpoint() const {
    const auto shown = m_shown == kHandOfDeath  ? Shown::HandOfDeath
                       : m_shown == kHealthVamp ? Shown::HealthVamp
                                                : Shown::None;
    return {m_handOfDeath, m_healthVamp, shown};
}
bool HeadGem::restore(const Checkpoint& state) {
    if (!state.valid()) {
        return false;
    }
    m_handOfDeath = state.handOfDeath;
    m_healthVamp = state.healthVamp;
    switch (state.shown) {
    case Shown::None: m_shown = {}; break;
    case Shown::HandOfDeath: m_shown = kHandOfDeath; break;
    case Shown::HealthVamp: m_shown = kHealthVamp; break;
    }
    return true;
}

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
