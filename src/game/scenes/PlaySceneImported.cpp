#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "game/combat/HurtKind.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/TowerGateways.h"
#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

bool PlayScene::importedOpening() const {
    const auto* imported = m_world ? m_world->imported() : nullptr;
    return imported != nullptr && (spawning() || !imported->opponentsReady() ||
                                   std::ranges::any_of(m_players, [](const PlayerRuntime& player) {
                                       return !player.departed &&
                                              player.life == PlayerLife::Standing &&
                                              player.figure && player.figure->animator().entering();
                                   }));
}

void PlayScene::openImportedDialogue() {
    auto* imported = m_world->imported();
    if (!imported || m_messages.active() || m_sumnerVisit.active()) {
        return;
    }
    const auto request = imported->takeDialogue();
    if (!request) {
        return;
    }
    if (!m_services->strings || !m_services->config) {
        imported->dismissDialogue();
        return;
    }
    const auto& strings = *m_services->strings;
    HintMenuLabels labels;
    if (!request->title.empty()) {
        labels.title = strings.get(request->title);
    }
    labels.player = strings.get("menu.player");
    if (const auto slot = labels.player.find("{}"); slot != std::string::npos) {
        labels.player.replace(slot, 2, std::to_string(request->player + 1));
    }
    for (const auto& topic : request->topics) {
        labels.topics.push_back({std::string{strings.get(topic.label)}, topic.code});
    }
    // A tower past the gateways offers them, offline, where its resident's menu takes the row.
    m_gatewayHere = {};
    if (request->gateways && m_links.gateways != nullptr) {
        const usize row = std::min(request->gateways->row, labels.topics.size());
        labels.topics.insert(labels.topics.begin() + static_cast<std::ptrdiff_t>(row),
                             MenuItem{std::string{strings.get(kGatewayTopicText)}, kGatewayTopic});
        m_gatewayHere = GatewayDestination{request->gateways->world};
    }
    const auto& config = *m_services->config;
    MenuScreen screen;
    screen.width = static_cast<s32>(config.display.virtualWidth);
    screen.height = static_cast<s32>(config.display.virtualHeight);
    screen.horizontalFov = config.horizontalFovRadians();
    const auto* style = imported->messagePresentation();
    const auto* art = style ? style->menuTextures() : nullptr;
    const auto& text = art ? style->text() : m_messages.text();
    const auto definition = art ? style->nativeTopics(labels.title, labels.topics) : std::nullopt;
    if (!m_sumnerVisit.openTopics(request->player, text, screen, std::move(labels), art, definition,
                                  art ? style : nullptr)) {
        imported->dismissDialogue();
    }
}

void PlayScene::updateImportedDialogue(const Inputs& inputs, s32 ticks) {
    auto* imported = m_world->imported();
    if (!imported) {
        return;
    }
    const auto owner = m_sumnerVisit.owner();
    MenuInput input = owner >= 0 && static_cast<usize>(owner) < inputs.size()
                          ? inputs[static_cast<usize>(owner)].menu
                          : MenuInput{};
    // Imported campaigns are offline. Resolve all their native topics directly;
    // the four-topic base campaign's online input encoding is not reused here.
    if (input.pointerNormalized) {
        if (m_presentedCanvas) {
            input = mapMenuPointer(std::move(input), *m_presentedCanvas);
        } else {
            input.pointer.reset();
        }
    }
    input.back |= input.escape || input.pointerBack;
    const auto event = m_sumnerVisit.update(*m_device, input, ticks);
    if (event.kind == HintMenuEvent::Kind::Asked && event.topic == kGatewayTopic &&
        m_links.gateways != nullptr) {
        openGatewayChoices();
    } else if (event.kind == HintMenuEvent::Kind::Chose) {
        takeGatewayChoice(event.topic);
        m_sumnerVisit.closeTopics();
        imported->dismissDialogue();
    } else if (event.kind == HintMenuEvent::Kind::Asked) {
        const auto keys = imported->answerDialogue(event.topic);
        m_sumnerVisit.closeTopics();
        if (m_services->strings) {
            std::vector<std::string> pages;
            pages.reserve(keys.size());
            for (const auto& key : keys) {
                pages.emplace_back(m_services->strings->get(key));
            }
            if (!pages.empty()) {
                m_messages.openImported(*m_device, pages, m_services->strings);
            }
        }
        imported->dismissDialogue();
    } else if (event.kind == HintMenuEvent::Kind::Left) {
        imported->dismissDialogue();
    }
}

void PlayScene::advanceImportedLevel(f32 seconds, std::vector<CameraSubject>& subjects,
                                     const Inputs& inputs) {
    auto* imported = m_world->imported();
    if (imported == nullptr) {
        return;
    }
    std::vector<s32> opponentFloors;
    const auto& enemies = m_opponents.enemies();
    for (s32 slot = 0; slot < enemies.most(); ++slot) {
        if (const s32 floor = enemies.floorOf(slot); floor >= 0) {
            opponentFloors.push_back(floor);
        }
    }
    imported->supportOpponents(opponentFloors);
    std::vector<PlayerActor*> standing;
    u32 interactionButtons = 0;
    for (auto& player : m_players) {
        if (!player.departed && player.life == PlayerLife::Standing && !player.ghost) {
            standing.push_back(&player.actor);
            const auto seat = player.actor.player();
            if (seat >= 0 && static_cast<usize>(seat) < inputs.size()) {
                const auto& input = inputs[static_cast<usize>(seat)];
                if (input.menu.select || input.attackPressed) {
                    interactionButtons |= 1U << seat;
                }
            }
        }
    }
    imported->interactionButtons(interactionButtons);
    imported->updateParty(standing, seconds,
                          m_services->config == nullptr ||
                              m_services->config->combat.autoActivateItems);
    openImportedDialogue();
    imported->affectArea(
        [&](const Vec3& position, f32 radius, f32 damage, u32 flags, std::vector<s32>& opponents) {
            m_opponents.blast(position, radius, damage, opponents, m_players, flags);
        });
    applyImportedHazards(*imported);
    presentImportedPickups(*imported);
    // Native transporters may relocate a visitor during this callback.
    // Frame the corrected body, not the pre-contact PartyMotion snapshot.
    for (usize i = 0; i < m_players.size(); ++i) {
        const auto& actor = m_players[i].actor;
        subjects[i] = {actor.position(), actor.followPoint(), actor.height() * .5f};
    }
    if (imported->travelRequested() && !m_sim.leaving) {
        m_sim.destination = {};
        m_sim.leaving = true;
        if (!imported->departure()) {
            m_departure.begin(*m_device, m_weapons.textures);
        }
        const auto native = imported->departure();
        if (!native || native->transportSound) {
            m_audio.playNamed(PortalDeparture::kSound, PortalDeparture::kVolume);
        }
    }
}

void PlayScene::holdImportedParty() {
    auto* imported = m_world->imported();
    if (imported == nullptr) {
        return;
    }
    std::vector<PlayerActor*> standing;
    for (auto& player : m_players) {
        if (!player.departed && player.life == PlayerLife::Standing && !player.ghost) {
            standing.push_back(&player.actor);
        }
    }
    imported->holdParty(standing);
}

/** The hazards an imported level's own traps dealt the party this tick. */
void PlayScene::applyImportedHazards(ImportedLevel& imported) {
    for (const auto& hit : imported.takeHazardHits()) {
        const auto victim = std::ranges::find_if(
            m_players, [&](const auto& runtime) { return runtime.actor.player() == hit.player; });
        if (victim != m_players.end()) {
            hurt(static_cast<usize>(victim - m_players.begin()), hit.damage,
                 hit.poison    ? HurtKind::Gas
                 : hit.pierces ? HurtKind::Pierce
                               : HurtKind::Blow,
                 hurtSourceOf(hit.impact.flags), hit.impact, hit.armorAbsorbed);
        }
    }
}

/** What the party took from an imported level, shown as the common pickups are: the
 * scroll a pickup opens, and the item's own presentation. */
void PlayScene::presentImportedPickups(ImportedLevel& imported) {
    for (const auto& receipt : imported.takePickups()) {
        const auto recipient = std::ranges::find_if(
            m_players, [&](const auto& player) { return player.actor.player() == receipt.player; });
        if (recipient != m_players.end()) {
            if (!receipt.scrollPages.empty()) {
                if (receipt.scrollKeys && m_services->strings) {
                    std::vector<std::string> pages;
                    pages.reserve(receipt.scrollPages.size());
                    for (const auto& key : receipt.scrollPages) {
                        pages.emplace_back(m_services->strings->get(key));
                    }
                    m_messages.openImported(*m_device, pages, m_services->strings);
                } else if (!receipt.scrollKeys) {
                    m_messages.openImported(*m_device, receipt.scrollPages, m_services->strings);
                }
            }
            // Native collection already granted the item. Reuse only the
            // common presentation, never the GC grant/campaign dispatcher.
            m_sim.pickups.presentTaken(receipt.offer, receipt.taking, receipt.name,
                                       static_cast<usize>(recipient - m_players.begin()), m_players,
                                       pickupServices());
        }
    }
}

} // namespace gdl::game
