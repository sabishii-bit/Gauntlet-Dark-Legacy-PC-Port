#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/hud/HudView.h"
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
    }
    return view;
}

} // namespace gdl::game
