#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/StringTable.h"

#include "game/party/PartyTeleport.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/GatewayDeparture.h"
#include "game/tower/TowerGateways.h"

namespace gdl::game {

bool PlayScene::offersGateways() const {
    return m_links.gateways != nullptr &&
           std::ranges::any_of(m_players, [](const PlayerRuntime& runtime) {
               return !runtime.departed && beatenGarm(runtime.actor.save());
           });
}

void PlayScene::openGatewayChoices() {
    if (m_links.gateways == nullptr || m_services->strings == nullptr) {
        return;
    }
    // The worlds past the gateways are looked for as the list is asked for, never before.
    m_gatewayChoices = gatewayChoices(m_links.gateways->worlds(), m_gatewayHere);
    std::vector<MenuItem> rows;
    rows.reserve(m_gatewayChoices.size());
    for (usize i = 0; i < m_gatewayChoices.size(); ++i) {
        const GatewayChoice& choice = m_gatewayChoices[i];
        MenuItem row{std::string(m_services->strings->get(choice.label)), static_cast<s32>(i)};
        row.enabled = choice.enabled;
        rows.push_back(std::move(row));
    }
    m_sumnerVisit.choose(std::move(rows));
}

bool PlayScene::takeGatewayChoice(s32 code) {
    if (code < 0 || static_cast<usize>(code) >= m_gatewayChoices.size()) {
        return false;
    }
    const GatewayChoice& choice = m_gatewayChoices[static_cast<usize>(code)];
    if (!choice.enabled) {
        return false;
    }
    m_gatewayDeparture.begin(choice.destination);
    return true;
}

bool PlayScene::advanceGatewayDeparture(s32 ticks) {
    if (!m_gatewayDeparture.holding()) {
        return false;
    }
    const GatewayDeparture::Step step = m_gatewayDeparture.update(ticks);
    if (step.sparkle && m_device != nullptr) {
        PartyTeleport::sparkle(*m_device, m_effects, m_weapons, m_players);
    }
    if (step.leave) {
        // Gone as the boss's fall sends a party on: the transition comes up over the view.
        m_sim.destination = {};
        m_sim.leaving = true;
        m_transition.comeUp();
    }
    return true;
}

bool PlayScene::cancelGatewayDeparture() {
    if (!m_open || !m_gatewayDeparture.active()) {
        return false;
    }
    if (m_gatewayDeparture.departed()) {
        m_sim.leaving = false;
        m_sim.destination = {};
        m_transition.clearAway();
    }
    m_gatewayDeparture.cancel();
    return true;
}

} // namespace gdl::game
