#include <algorithm>
#include <array>
#include <format>

#include "game/screens/PlayScene.h"

namespace gdl::game {
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
    m_promotion.animate(seconds);
    m_world->update(seconds);
    m_sumner.update(seconds);
    m_effects.update(seconds);
    updateAmbience();
    if (entering) {
        return;
    }
    const auto* entry = m_promotion.current();
    if (entry == nullptr) {
        return;
    }
    auto& runtime = m_players[entry->player];
    auto& save = runtime.actor.save();
    const auto cue = m_promotion.update(ticks, m_context.sounds != nullptr &&
                                                   m_context.sounds->isPlaying(m_promotionVoice));
    if (cue.voice) {
        SoundHandle name = kNoSound;
        if (runtime.figure != nullptr) {
            name = m_audio.playFrom(runtime.figure->voice(),
                                    std::format("S_{}{}2", colorCode(save.color),
                                                classCode(save.character % kStartingClassCount)));
        }
        m_promotionVoice = m_audio.playPromotion(entry->voice, name);
    }
    if (cue.award) {
        save.progress().promotedLevel = entry->level;
        if (auto figure = PlayerFigure::load(*m_device, m_context.unpackedRoot, save, false);
            figure != nullptr) {
            m_promotionFigures.push_back(std::move(runtime.figure));
            runtime.figure = std::move(figure);
        }
        EffectTrees::Setting setting;
        setting.seconds = 3;
        m_effects.startSet(*m_device, m_weapons, std::format("LEVELUP_{}", colorCode(save.color)),
                           runtime.actor.position(), setting);
    }
    if (cue.gem) {
        constexpr std::array<std::string_view, kPlayerCount> kGems{"GETGEMYELLOW", "GETGEMBLUE",
                                                                   "GETGEMRED", "GETGEMGREEN"};
        m_effects.start(*m_device, m_world->powerups(), kGems[static_cast<usize>(save.color)],
                        runtime.actor.position());
    }
}
} // namespace gdl::game
