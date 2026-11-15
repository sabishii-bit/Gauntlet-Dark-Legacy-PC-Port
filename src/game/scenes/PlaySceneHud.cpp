#include <optional>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/hud/HudView.h"
#include "game/level/ExitPortals.h"
#include "game/level/ImportedLevel.h"
#include "game/level/SecretChallenge.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

HudView PlayScene::hudView(const Mat4& clip, const Mat4& canvasProjection, f32 width, f32 height,
                           bool legends) const {
    HudView view;
    view.players = m_players;
    view.relicsShown = m_hud.relicsShown();
    view.quitOffered = !m_sim.fixedRoster;
    view.seconds = m_sim.playSeconds;
    view.pickups = &m_hud.pickups();
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const auto index = static_cast<s32>(seat);
        const HudSeatRules rules{.keysShown = view.relicsShown,
                                 .quitOffered = view.quitOffered,
                                 .local = ((m_localSeats >> seat) & 1U) != 0};
        view.seats[seat] = hudSeatOf(index, m_players, m_hud.selector(index), m_classes, rules);
        if (view.seats[seat].runtime != nullptr && !view.seats[seat].departed) {
            ++view.joined;
        }
    }
    view.help = hudHelpOf(m_hud.help(), m_hud.helpAnchor(m_players), m_players, clip,
                          canvasProjection, width, height);
    // Stop Time's sand, worn, takes the secret level's timer's place.
    if (const auto stopTime = m_hud.hourglassLook(m_players)) {
        view.hourglass = stopTime;
        view.hourglassArt = &m_hud.hourglass();
    } else if (m_sim.challenge.state() != SecretChallenge::State::Inactive) {
        view.hourglass =
            m_challengeHud.look(m_sim.challenge.remaining(), m_sim.challenge.duration(),
                                !spawning() && !m_messages.active());
        view.hourglassArt = &m_challengeHud;
    }
    view.runeFill = m_sim.runeMeter.shownFill();
    view.bossMeters = &m_opponents.meter();
    view.strings = m_services->strings;
    view.largeText = &m_messages.text();
    if (legends) {
        view.bossCount = hudBossReadingsOf(m_opponents.bosses().healthMeters(), view.bossReadings);
        if (const ImportedLevel* imported = m_world != nullptr ? m_world->imported() : nullptr) {
            // An imported level's exits are its own, and so is who waits on them.
            const auto exits = importedExitsOf(*imported);
            for (usize seat = 0; seat < view.seats.size(); ++seat) {
                const std::optional<ImportedExit>& exit = exits.at(seat);
                view.seats[seat].waitingAtExit = exit.has_value() && exit->waiting;
                view.seats[seat].pad = exit ? importedPadOf(*exit) : std::nullopt;
            }
            view.levelHud = imported->hudStyleId();
        } else if (m_world != nullptr) {
            // Those the portals wait for, as advancePortals stands them.
            std::vector<PortalVisitor> standing;
            standing.reserve(m_players.size());
            for (usize i = 0; i < m_players.size(); ++i) {
                if (!isDown(i) && !m_players[i].ghost) {
                    standing.push_back(PortalVisitor{.position = m_players[i].actor.position(),
                                                     .radius = m_players[i].actor.radius(),
                                                     .partyIndex = static_cast<s32>(i)});
                }
            }
            const auto pads = darkLegacyPadsOf(m_portals, m_world->ref(), standing, m_players);
            for (usize seat = 0; seat < view.seats.size(); ++seat) {
                view.seats[seat].pad = pads.at(seat);
            }
        }
    }
    return view;
}

} // namespace gdl::game
