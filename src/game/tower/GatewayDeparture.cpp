#include "game/tower/GatewayDeparture.h"

#include <algorithm>

namespace gdl::game {

void GatewayDeparture::begin(GatewayDestination destination) {
    m_stage = Stage::Asked;
    m_destination = destination;
    m_ticksLeft = 0;
}

GatewayDeparture::Step GatewayDeparture::update(s32 ticks) {
    Step step;
    switch (m_stage) {
    case Stage::Asked:
        // The sparkle comes as the party is free of the scroll, its ticks counted from there.
        m_stage = Stage::Sparkling;
        m_ticksLeft = kSparkleTicks;
        step.sparkle = true;
        break;
    case Stage::Sparkling:
        m_ticksLeft -= std::max(ticks, 0);
        if (m_ticksLeft <= 0) {
            m_stage = Stage::Gone;
            step.leave = true;
        }
        break;
    case Stage::None:
    case Stage::Gone: break;
    }
    return step;
}

void GatewayDeparture::cancel() {
    m_stage = Stage::None;
    m_destination.reset();
    m_ticksLeft = 0;
}

} // namespace gdl::game
