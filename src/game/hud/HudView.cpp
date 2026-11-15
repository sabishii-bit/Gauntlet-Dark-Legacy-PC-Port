#include "game/hud/HudView.h"

#include <algorithm>

#include "game/characters/CharacterSave.h"
#include "game/characters/Vitality.h"
#include "game/hud/PartyHud.h"
#include "game/party/PartyNames.h"

namespace gdl::game {

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
