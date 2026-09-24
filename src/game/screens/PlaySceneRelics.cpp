#include <algorithm>

#include "game/screens/PlayScene.h"

namespace gdl::game {
bool PlayScene::relicCeremonyOn() const {
    return m_towerRelics.active() && !spawning() && !m_promotion.active() &&
           m_intro != Intro::Crystal && m_intro != Intro::Scroll;
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
    if (cue.placement) {
        const auto* entry = m_towerRelics.current();
        constexpr u16 kCompleteWindow = 0x1fe;
        std::string_view sound = "S_RUNEFALL";
        if (entry->kind == TowerRelics::Kind::Shard) {
            // The shipped dispatch uses these names in this order despite
            // their counterintuitive suffixes: completion selects S_SHRDS127.
            sound = (m_towerRelics.displayedShards() | entry->bit()) == kCompleteWindow
                        ? "S_SHRDS127"
                        : "S_SHRD8";
        }
        m_audio.playPromotion(sound);
    }
    if (cue.completed) {
        for (auto& runtime : m_players) {
            TowerRelics::acknowledge(runtime.actor.save().progress().relics, *cue.completed);
        }
    }
}
} // namespace gdl::game
