#include <utility>

#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

bool LegendsLevel::updateDreamcastSumner(std::span<PlayerActor*> players, f32 seconds,
                                         bool blocked) {
    const u32 pressed = std::exchange(m_interactionButtons, 0);
    if (!m_dreamcastSumner) {
        return false;
    }
    std::vector<DreamcastSumnerVisitor> visitors;
    for (const auto* actor : players) {
        if (actor) {
            const auto seat = actor->player();
            visitors.push_back(
                {seat, actor->position(), seat >= 0 && seat < 4 && (pressed & (1U << seat)) != 0});
        }
    }
    const auto owner = m_dreamcastSumner->update(visitors, seconds, blocked || m_dialogueOpen);
    m_world.adapter()->faceDreamcastSumner(m_dreamcastSumner->yaw());
    if (owner) {
        m_dialogue = ImportedDialogue{*owner,
                                      {},
                                      {{0, "legends.dreamcast.sumner.topic.history"},
                                       {1, "legends.dreamcast.sumner.topic.obelisk"},
                                       {2, "legends.dreamcast.sumner.topic.rune"},
                                       {3, "legends.dreamcast.sumner.topic.enemy"},
                                       {4, "legends.dreamcast.sumner.topic.exit"}}};
        m_dialogueOpen = true;
    }
    return m_dialogueOpen;
}

} // namespace gdl::game
