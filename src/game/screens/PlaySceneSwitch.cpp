#include "engine/core/Types.h"

#include "game/screens/PlayScene.h"

namespace gdl::game {

void PlayScene::updateSwitchCutscene(s32 ticks, f32 seconds) {
    // The scenery and activated switch keep animating; combat clocks, projectiles,
    // generators, damage, inventory timers and player input do not advance.
    m_world->update(seconds);
    m_world->updateTriggers(seconds, visitors());
    m_fixtures.syncFloors();
    m_opponents.syncFloors();
    m_portals.animate(seconds);
    m_transporters.animate(seconds);
    handleTriggerEvents();
    m_switchCutscene.update(ticks, m_world->triggers().settled(m_switchCutscene.target()));
    if (!m_switchCutscene.active()) {
        std::vector<CameraSubject> subjects;
        for (const auto& player : m_players) {
            if (player.life == PlayerLife::Standing) {
                subjects.push_back({player.actor.position(), player.actor.followPoint()});
            }
        }
        m_camera.snapAttention(subjects, m_world->cameraRange());
    }
    updateAmbience();
}
} // namespace gdl::game
