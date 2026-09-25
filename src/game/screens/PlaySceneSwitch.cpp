#include "engine/core/Types.h"

#include "game/screens/PlayScene.h"

namespace gdl::game {

void PlayScene::updateSwitchCutscene(s32 ticks, f32 seconds) {
    // The scenery and activated switch keep animating; combat clocks, projectiles,
    // generators, damage, inventory timers and player input do not advance.
    m_world->update(seconds);
    m_world->updateTriggers(seconds, visitors());
    m_portals.animate(seconds);
    m_transporters.animate(seconds);
    handleTriggerEvents();
    m_switchCutscene.update(ticks, m_world->triggers().settled(m_switchCutscene.target()));
    updateAmbience();
}
} // namespace gdl::game
