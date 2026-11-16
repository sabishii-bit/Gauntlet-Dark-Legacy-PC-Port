#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/characters/CharacterSave.h"
#include "game/config/GameConfig.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/PowerupSelector.h"
#include "game/level/ImportedLevel.h"
#include "game/level/SecretChallenge.h"
#include "game/party/PartyCombo.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerControls.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

HudView PlayScene::hudView(const Mat4& clip, const Mat4& canvasProjection, f32 width, f32 height,
                           bool otherLook) const {
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
    if (otherLook) {
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
            // Each seat's pad among those the portals wait for, as they count them.
            const auto pads =
                darkLegacyPadsOf(m_portals, m_world->ref(), portalVisitors(), m_players);
            for (usize seat = 0; seat < view.seats.size(); ++seat) {
                view.seats[seat].pad = pads.at(seat);
            }
        }
    }
    return view;
}

std::string_view PlayScene::configuredHudStyle() const {
    return m_services->config != nullptr ? std::string_view{m_services->config->display.hudStyle}
                                         : kDarkLegacyHudStyle;
}

std::vector<HudMember> PlayScene::hudMembers() const {
    std::vector<HudMember> members;
    members.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        const s32 seat = runtime.actor.player();
        if (seat >= 0 && seat < kPlayerCount) {
            const CharacterSave& save = runtime.actor.save();
            members.push_back({.seat = seat, .classIndex = save.character, .color = save.color});
        }
    }
    return members;
}

bool PlayScene::hudBrowsable(s32 seat) const {
    if (!m_open || m_world == nullptr) {
        return false;
    }
    // A hold that owns the tick heeds no selector: the console's journey, then advanceNative's
    // holds in their order, then those that hold play's party still.
    if (m_sim.consoleTravel.has_value() || m_sim.gameOver.active() ||
        m_sim.switchCutscene.active() || m_messages.active() || m_sim.leaving ||
        m_sumnerVisit.active() || spawning() ||
        (m_ceremonies.promotion().active() && !m_sim.welcome.cutting() && !relicsLead()) ||
        relicCeremonyOn() || m_sim.welcome.holds() || importedOpening() ||
        m_gatewayDeparture.holding()) {
        return false;
    }
    // Nor does a member down, gone or entering, held or thrown, moved by a combo, or flying
    // free, whose presses never reach the game.
    const PlayerRuntime* member = runtime(seat);
    return member != nullptr && !member->departed && member->life == PlayerLife::Standing &&
           (member->figure == nullptr || !member->figure->animator().entering()) &&
           !member->capture.active() && !PartyCombo::aside(*member) && !cheats(seat).noclip;
}

void PlayScene::browseHud(s32 seat, const SelectorInput& presses) {
    if (seat < 0 || seat >= kPlayerCount || ((m_localSeats >> seat) & 1U) == 0 ||
        !(presses.down || presses.left || presses.right) || !hudBrowsable(seat) ||
        m_hud.selector(seat).state() != PowerupSelector::State::Closed) {
        return;
    }
    m_hudStyle.run(m_darkLegacyHud, [&](HudPainter& look) { look.browse(seat, presses); });
}

} // namespace gdl::game
