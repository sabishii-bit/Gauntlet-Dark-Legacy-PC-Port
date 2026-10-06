#include "engine/core/Types.h"

#include "game/screens/PlayScene.h"

namespace gdl::game {

void PlayScene::updateSwitchCutscene(s32 ticks, f32 seconds) {
    // A switch camera disables controls, not the player's action machine. In-flight
    // actions finish and the idle pose keeps moving; enemy AI and inventory clocks wait.
    m_world->update(seconds);
    m_world->updateTriggers(seconds, visitors(), true);
    // World item placement still runs while controls and combat are held.
    // The shot can reveal a placed enemy that the follow camera has never seen.
    watchOpponents();
    m_fixtures.syncFloors();
    m_opponents.syncFloors();
    const auto subjects =
        PartyMotion::step(m_players, {}, true, bossCameraOn() ? m_bossCamera.yaw() : m_camera.yaw(),
                          ticks, seconds, m_world->collision(), motionEvents());
    m_shake.update(ticks);
    m_attacks.updateProjectiles(seconds, m_players, attackTargets());
    m_projectilesAdvanced = seconds > 0;
    m_attacks.updateStrikes(seconds, m_players, attackTargets());
    m_dimmer.update(seconds);
    m_world->setAmbientOffset(m_dimmer.offset());
    m_effects.update(seconds);
    m_portals.animate(seconds);
    m_transporters.animate(seconds);
    handleTriggerEvents();
    m_switchCutscene.update(ticks, m_world->triggers().settled(m_switchCutscene.target()));
    if (!m_switchCutscene.active()) {
        std::vector<CameraSubject> standing;
        for (usize i = 0; i < m_players.size(); ++i) {
            if (m_players[i].life == PlayerLife::Standing) {
                standing.push_back(subjects[i]);
            }
        }
        m_camera.snapAttention(standing, m_world->cameraRange());
    }
    updateAmbience();
}
} // namespace gdl::game
