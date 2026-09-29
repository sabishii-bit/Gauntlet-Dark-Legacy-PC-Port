#include "game/players/MeleeStreak.h"

#include "engine/core/Types.h"

namespace gdl::game {

void MeleeStreak::record(bool telling) {
    m_count += telling ? kTelling : kHurt;
    m_quiet = 0.0f;
    m_running = true;
}

std::string_view MeleeStreak::step(f32 seconds) {
    if (!m_running) {
        return {};
    }
    m_quiet += seconds;
    if (m_quiet <= kQuiet) {
        return {};
    }
    std::string_view praise;
    if (m_count >= kBravery) {
        praise = kBraveryVoice;
    } else if (m_count >= kHeroic) {
        praise = kHeroicVoice;
    }
    m_count = 0;
    m_quiet = 0.0f;
    m_running = false;
    return praise;
}

} // namespace gdl::game
