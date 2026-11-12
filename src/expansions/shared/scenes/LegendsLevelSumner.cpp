#include <utility>

#include "expansions/dreamcast/level/LegendsDreamcastTower.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

bool LegendsLevel::updateSumner(std::span<PlayerActor*> players, f32 seconds, bool blocked) {
    if (m_dreamcastSumner) {
        return updateDreamcastSumner(players, seconds, blocked);
    }
    return updateN64Sumner(players, seconds, blocked);
}

std::optional<ImportedDialogue> LegendsLevel::takeDialogue() {
    return std::exchange(m_dialogue, {});
}

std::vector<std::string> LegendsLevel::answerDialogue(s32 topic) {
    if (m_dreamcastSumner && m_dialogueOpen) {
        return LegendsDreamcastTower::advice(topic, adapter().partyProgress());
    }
    return answerN64Dialogue(topic);
}

void LegendsLevel::dismissDialogue() {
    m_dialogue.reset();
    m_dialogueOpen = false;
}

bool LegendsLevel::takeShopRequest() {
    return std::exchange(m_shopRequested, false);
}

bool LegendsLevel::placeOpeningParty(std::span<PlayerActor*> party) {
    // Of the native visits, only the N64 Tower's ceremonies open on a script.
    return platform() == LegendsPlatform::N64 && placeN64OpeningParty(party);
}

void LegendsLevel::holdParty(std::span<PlayerActor* const> party) {
    // Of the native scripts that hold play, only the N64 Tower's ceremonies turn the party.
    if (platform() == LegendsPlatform::N64) {
        holdN64Party(party);
    }
}

} // namespace gdl::game
