#include <algorithm>

#include "game/screens/PlayScene.h"

namespace gdl::game {
bool PlayScene::relicCeremonyOn() const {
    return m_towerRelics.active() && !spawning() && !m_promotion.active() &&
           m_welcome.intro() != Intro::Crystal && m_welcome.intro() != Intro::Scroll;
}
void PlayScene::beginTowerRelics() {
    std::vector<Relics> collection;
    Vec3 centre{0};
    for (const auto& runtime : m_players) {
        collection.push_back(runtime.actor.save().progress().relics);
        centre += runtime.actor.position();
    }
    if (!m_players.empty()) {
        centre /= static_cast<f32>(m_players.size());
    }
    m_towerRelics.begin(collection, m_messages.strings());
    m_towerRelics.bind(*m_device, *m_world, centre);
    // If another party member has already installed a piece, nobody needs to
    // replay it. The acquisition itself remains in each character's save.
    for (auto& runtime : m_players) {
        auto& relics = runtime.actor.save().progress().relics;
        relics.pendingRunes &= static_cast<u16>(~m_towerRelics.displayedRunes());
        relics.pendingShards &= static_cast<u16>(~m_towerRelics.displayedShards());
        relics.pendingCeremonies = m_towerRelics.pendingCeremonies();
        // The temple boss has no window piece. Its return can instead complete
        // the Underworld route, now retained by the follow-up's pending bit.
        relics.pendingShards &= static_cast<u16>(~(1U << 9));
    }
    revealTowerRoutes();
}
void PlayScene::revealTowerRoutes() {
    for (const auto kind : {TowerCompletion::Kind::Window, TowerCompletion::Kind::Underworld,
                            TowerCompletion::Kind::Garm}) {
        const auto tag = TowerCompletion::portal(kind);
        const f32 alpha = m_towerRelics.revealAlpha(kind);
        m_portals.setAlpha(tag, alpha);
        const auto glow = TowerAccess::glowObjectName(TowerAccess::worldOfLetter(tag[0]),
                                                      ExitPortals::gateOf(tag));
        for (usize i = 0; i < m_portals.size(); ++i) {
            if (m_portals.portal(i).tag != tag || m_portals.portal(i).shut) {
                continue;
            }
            const auto& objects = m_world->layout().objects();
            for (usize object = 0; object < objects.size(); ++object) {
                if (objects[object].name == glow) {
                    m_world->setObjectAlpha(object, alpha);
                }
            }
        }
    }
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
    m_world->update(seconds);
    m_world->updateTriggers(seconds, {});
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
    const auto cue = m_towerRelics.update(
        ticks, seconds, m_context.sounds != nullptr && m_context.sounds->isPlaying(m_relicVoice));
    if (!cue.voice.empty()) {
        m_relicVoice = m_audio.playPromotion(cue.voice);
    }
    if (!cue.sound.empty()) {
        m_audio.playPromotion(cue.sound);
    }
    if (cue.completed) {
        for (auto& runtime : m_players) {
            TowerRelics::acknowledge(runtime.actor.save().progress().relics, *cue.completed);
        }
    }
    revealTowerRoutes();
}
} // namespace gdl::game
