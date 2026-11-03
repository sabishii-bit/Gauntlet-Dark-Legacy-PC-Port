#include "engine/core/Types.h"

#include "game/party/PlayerPowerups.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

void PlayScene::updateSwitchCutscene(s32 ticks, f32 seconds) {
    // A switch camera disables controls, not the player's action machine. In-flight
    // actions finish and the idle pose keeps moving; enemy AI and inventory clocks wait.
    // DoWorldAnimSub checks Stop Time even while a trigger camera is active;
    // only looping tracks wait, so the one-shot target can still finish its shot.
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_world->updateTriggers(seconds, visitors(), true, occupiedFloors());
    // World item placement still runs while controls and combat are held.
    // The shot can reveal a placed enemy that the follow camera has never seen.
    watchOpponents();
    m_fixtures.syncFloors();
    m_opponents.syncFloors();
    const auto subjects =
        stepParty({}, true, movementYaws(bossCameraOn() ? m_sim.bossCamera.yaw() : m_camera.yaw()),
                  ticks, seconds);
    m_sim.shake.update(ticks);
    m_attacks.updateProjectiles(seconds, m_players, attackTargets());
    m_sim.projectilesAdvanced = seconds > 0;
    m_attacks.updateStrikes(seconds, m_players, attackTargets());
    m_sim.dimmer.update(seconds);
    m_world->setAmbientOffset(m_sim.dimmer.offset());
    m_effects.update(seconds);
    m_portals.animate(seconds);
    m_transporters.animate(seconds);
    handleTriggerEvents();
    m_sim.switchCutscene.update(ticks, m_world->triggers().settled(m_sim.switchCutscene.target()));
    if (!m_sim.switchCutscene.active()) {
        std::vector<CameraSubject> standing;
        for (usize i = 0; i < m_players.size(); ++i) {
            if (m_players[i].life == PlayerLife::Standing) {
                standing.push_back(subjects[i]);
            }
        }
        m_camera.snapAttention(standing, m_world->cameraRange());
        if (m_sim.soloCameras) {
            snapSeatCameras(subjects);
        }
    }
    updateAmbience();
}
} // namespace gdl::game
