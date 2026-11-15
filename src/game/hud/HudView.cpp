#include "game/hud/HudView.h"

#include <algorithm>

#include "game/characters/CharacterSave.h"
#include "game/characters/Vitality.h"
#include "game/hud/PartyHud.h"
#include "game/party/PartyNames.h"

namespace gdl::game {
namespace {

/** The tower, where an ordinary stage's exits take the party, for the pads that name it. */
const LevelRef& towerLevel() {
    static const LevelRef kTower = LevelRef::tower();
    return kTower;
}

} // namespace

static_assert(HudView::kSeats == static_cast<usize>(PartyHud::kPlayerCount));

HudSeat hudSeatOf(s32 seat, std::span<const PlayerRuntime> players, const PowerupSelector& selector,
                  const ClassDataSet& classes, const HudSeatRules& rules) {
    HudSeat shown;
    shown.box = PartyHud::status(seat, players, &selector);
    shown.box.keysShown = rules.keysShown;
    shown.box.quitOffered = rules.quitOffered;
    shown.selector = &selector;
    shown.local = rules.local;
    const auto found = std::ranges::find_if(
        players, [seat](const PlayerRuntime& runtime) { return runtime.actor.player() == seat; });
    if (found == players.end()) {
        return shown;
    }
    const PlayerRuntime& runtime = *found;
    const CharacterSave& save = runtime.actor.save();
    shown.runtime = &runtime;
    shown.departed = runtime.departed;
    if (!runtime.departed && runtime.life == PlayerLife::Standing) {
        shown.turbo = runtime.turbo.shown();
    }
    shown.mostHealth = mostHealth(experienceLevel(save.experience()));
    if (const ClassStats* stats = classes.stats(save.character)) {
        shown.stats = characterStats(*stats, save);
    }
    shown.experience = save.experience();
    shown.legends = save.progress().relics.legends;
    shown.it = runtime.itTicks > 0;
    shown.waitingAtExit = runtime.exitWait.ticks() > 0;
    return shown;
}

std::array<std::optional<HudPad>, HudView::kSeats>
darkLegacyPadsOf(const ExitPortals& portals, const LevelRef& here,
                 std::span<const PortalVisitor> standing, std::span<const PlayerRuntime> players) {
    std::array<std::optional<HudPad>, HudView::kSeats> pads;
    for (const PortalVisitor& visitor : standing) {
        if (visitor.partyIndex < 0 || static_cast<usize>(visitor.partyIndex) >= players.size()) {
            continue;
        }
        const s32 seat = players[static_cast<usize>(visitor.partyIndex)].actor.player();
        if (seat < 0 || static_cast<usize>(seat) >= pads.size()) {
            continue;
        }
        if (const auto index = portals.padUnder(visitor, standing)) {
            const ExitPortals::Portal& exit = portals.portal(*index);
            pads.at(static_cast<usize>(seat)) =
                HudPad{.identity = kDarkLegacyPadIdentityBase + static_cast<u32>(*index),
                       .nativeCode = std::nullopt,
                       .destination = ExitPortals::returnsToTower(here, exit) ? &towerLevel()
                                                                              : &*exit.destination};
        }
    }
    return pads;
}

std::array<std::optional<ImportedExit>, HudView::kSeats>
importedExitsOf(const ImportedLevel& level) {
    std::array<std::optional<ImportedExit>, HudView::kSeats> exits;
    for (usize seat = 0; seat < exits.size(); ++seat) {
        exits.at(seat) = level.exitUnder(static_cast<s32>(seat));
    }
    return exits;
}

std::optional<HudPad> importedPadOf(const ImportedExit& exit) {
    if (!exit.code || *exit.code == 0) {
        return std::nullopt;
    }
    return HudPad{.identity = *exit.code, .nativeCode = exit.code, .destination = nullptr};
}

usize hudBossReadingsOf(std::span<const HealthMeterReading> readings,
                        std::array<HudBossReading, BossMeters::kCapacity>& out) {
    usize count = 0;
    for (const HealthMeterReading& reading : readings) {
        if (!reading.definition.shown || reading.definition.pieces <= 0) {
            continue;
        }
        if (count == out.size()) {
            break;
        }
        out.at(count++) = {.health = reading.health, .maxHealth = reading.maxHealth};
    }
    std::ranges::fill(std::span{out}.subspan(count), HudBossReading{});
    return count;
}

std::optional<HudHelp> hudHelpOf(const HelpMessages& help, std::optional<Vec3> anchor,
                                 std::span<const PlayerRuntime> players, const Mat4& clip,
                                 const Mat4& canvasProjection, f32 width, f32 height) {
    if (!help.showing()) {
        return std::nullopt;
    }
    HudHelp shown;
    shown.lines = help.lines();
    shown.player = help.player();
    shown.head = Vec2{width * 0.5f, height * 0.5f};
    if (anchor) {
        if (const auto head = PartyNames::screenOf(clip, *anchor, canvasProjection)) {
            shown.head = *head;
            shown.anchored = true;
        }
    }
    if (shown.player >= 0) {
        const auto found = std::ranges::find_if(players, [&shown](const PlayerRuntime& runtime) {
            return runtime.actor.player() == shown.player;
        });
        if (found != players.end()) {
            shown.color = found->actor.save().color;
        }
    }
    return shown;
}

} // namespace gdl::game
