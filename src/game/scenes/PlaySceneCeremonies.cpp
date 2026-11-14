#include <algorithm>

#include "game/party/PlayerPowerups.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

bool PlayScene::ceremoniesWaiting() const {
    return spawning() || m_messages.active();
}

bool PlayScene::relicCeremonyOn() const {
    return m_ceremonies.relics().active() && !ceremoniesWaiting() &&
           (!m_ceremonies.promotion().active() || relicsLead()) &&
           m_sim.welcome.intro() != Intro::Crystal;
}

bool PlayScene::relicsLead() const {
    // TowerCheckMessages ends the speech it opens a route with at once (SumnerSpeechEnd,
    // 0x800A30A4), so the route's reveal plays while the level-ups wait for the speech timer
    // the reveal sets (0x800A2B50 to 0x800A2B84).
    return m_ceremonies.relics().phase() == TowerRelics::Phase::Reveal;
}

TowerCeremonies::Scene PlayScene::ceremonyScene() {
    return {
        *m_device, *m_world, m_players, m_audio, m_effects, m_weapons, m_services->unpackedRoot};
}

s32 PlayScene::familiarTier(s32 player) const {
    for (const auto& runtime : m_players) {
        if (runtime.actor.player() == player && runtime.figure != nullptr) {
            return runtime.figure->familiarTier();
        }
    }
    return 0;
}

void PlayScene::updatePromotion(s32 ticks, f32 seconds) {
    // The spawn effect can finish before a class's START sequence. Hold the speech
    // as well as input until every standing body has settled into its stance.
    const bool entering = std::ranges::any_of(m_players, [](const PlayerRuntime& player) {
        return player.figure != nullptr && player.figure->animator().entering();
    });
    for (auto& player : m_players) {
        if (player.figure != nullptr) {
            player.figure->animate(0, ticks, seconds);
        }
    }
    m_ceremonies.promotion().animate(seconds);
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_sumner.update(seconds);
    m_effects.update(seconds);
    updateAmbience();
    if (entering) {
        return;
    }
    m_ceremonies.advancePromotion(ceremonyScene(), ticks);
}

void PlayScene::updateTowerRelics(s32 ticks, f32 seconds) {
    const bool entering = std::ranges::any_of(m_players, [](const PlayerRuntime& runtime) {
        return runtime.figure != nullptr && runtime.figure->animator().entering();
    });
    for (auto& runtime : m_players) {
        if (runtime.figure != nullptr) {
            runtime.figure->animate(0, ticks, seconds);
        }
    }
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_world->updateTriggers(seconds, {}, false, occupiedFloors());
    for (const auto& opening : m_world->takeTriggerOpenings()) {
        m_audio.opening(opening);
    }
    for (const auto& settled : m_world->takeTriggerSettled()) {
        m_audio.settled(settled);
    }
    // The ceremony owns these views; do not replay a trigger cut afterwards.
    m_world->takeTriggerCameraCues();
    m_sumner.update(seconds);
    m_effects.update(seconds);
    updateAmbience();
    if (entering) {
        return;
    }
    m_ceremonies.advanceRelics(ceremonyScene(), m_portals, ticks, seconds);
}

} // namespace gdl::game
