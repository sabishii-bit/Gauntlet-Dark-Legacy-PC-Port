#include "game/players/ExitWait.h"

#include "engine/core/Types.h"

namespace gdl::game {

bool ExitWait::step(bool waiting, s32 ticks) {
    if (!waiting) {
        m_ticks = 0;
        return false;
    }
    m_ticks += ticks;
    if (m_ticks < kFirstTicks) {
        return false;
    }
    m_ticks -= kAgainTicks;
    return true;
}

} // namespace gdl::game
