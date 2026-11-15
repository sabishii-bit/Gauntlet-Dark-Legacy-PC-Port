#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/TextPainter.h"

#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/level/ExitPortals.h"
#include "game/level/ImportedLevel.h"
#include "game/level/LevelCatalog.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {

/** The base of the identities the HUD gives Dark Legacy's exits, clear of every imported
 * level's 16-bit destination codes. */
inline constexpr u32 kDarkLegacyPadIdentityBase = 0x10000;

/** The exit a seat stands on. */
struct HudPad {
    u32 identity = 0;                      ///< non-zero; another exit, another identity
    std::optional<u32> nativeCode;         ///< an imported level's own destination code
    const LevelRef* destination = nullptr; ///< a Dark Legacy exit's level; borrowed for the frame
};

/** One seat as the HUD shows it this frame. What it points to is borrowed for the frame. */
struct HudSeat {
    StatusBoxView box;                         ///< its status box, with the frame's rules applied
    const PlayerRuntime* runtime = nullptr;    ///< none for a seat nobody plays
    const PowerupSelector* selector = nullptr; ///< the selector over its box
    std::optional<f32> turbo;                  ///< the turbo shown, 0 to 100, for a standing player
    s32 mostHealth = 0;                        ///< full health at the character's level
    StatBlock stats;                           ///< strength, speed, armour, magic
    s32 experience = 0;
    u16 legends = 0;            ///< the legend items held, a bit per realm
    bool local = false;         ///< played on this machine
    bool departed = false;      ///< quit from the prompt: out of the party, seat kept
    bool it = false;            ///< tagged IT
    bool waitingAtExit = false; ///< on an exit, waiting for the rest of the party
    std::optional<HudPad> pad;  ///< the exit it stands on, for a look that names it
};

/** One bound boss meter's health, for a look that eases its meters its own way. */
struct HudBossReading {
    f32 health = 0;
    f32 maxHealth = 0;
};

/** The help message up this frame. */
struct HudHelp {
    std::span<const std::string> lines;
    s32 player = -1;       ///< the seat it is for; -1 for a message at a place
    s32 color = -1;        ///< that seat's costume colour; -1 for none
    Vec2 head{0};          ///< as Dark Legacy places it: the anchor projected, else the centre
    bool anchored = false; ///< head is a projected point, not the canvas centre
};

/** Everything a HUD painter reads in one frame, built from the scene's digested owners.
 * A painter never keeps it, or anything it points to, past the frame. */
struct HudView {
    static constexpr usize kSeats = 4; ///< the boxes along the bottom, one a seat
    std::array<HudSeat, kSeats> seats;
    std::span<const PlayerRuntime> players; ///< the party in its own order
    s32 joined = 0;                         ///< seats with a player in the game
    bool relicsShown = false;
    bool quitOffered = true;
    f32 seconds = 0; ///< the scene's play clock; it runs back on a rewind
    const PickupHud* pickups = nullptr;
    std::optional<HudHelp> help;
    std::optional<ChallengeHud::Look> hourglass;
    const ChallengeHud* hourglassArt = nullptr; ///< Dark Legacy's art for that look
    std::optional<f32> runeFill;                ///< the runestone finder's fill, while it shows
    const BossMeters* bossMeters = nullptr;
    std::array<HudBossReading, BossMeters::kCapacity> bossReadings{};
    usize bossCount = 0;       ///< how many of bossReadings are bound
    std::string_view levelHud; ///< the look native to the level in play; empty for Dark Legacy's
    const StringTable* strings = nullptr;
    const TextPainter* largeText = nullptr; ///< FONT32, which the selector labels use
};

/** What a frame's rules make of one seat's box, and whether this machine plays the seat. */
struct HudSeatRules {
    bool keysShown = false;  ///< the bosses' keys show a while
    bool quitOffered = true; ///< a fallen player may quit the game
    bool local = false;
};

/** Seat `seat`'s part of the view; an empty seat when nobody plays it. */
HudSeat hudSeatOf(s32 seat, std::span<const PlayerRuntime> players, const PowerupSelector& selector,
                  const ClassDataSet& classes, const HudSeatRules& rules);
/** Each seat's Dark Legacy exit. `standing` are the visitors advancePortals builds, each read
 * as its runtime's seat; the identity is kDarkLegacyPadIdentityBase plus the portal's index. */
std::array<std::optional<HudPad>, HudView::kSeats>
darkLegacyPadsOf(const ExitPortals& portals, std::span<const PortalVisitor> standing,
                 std::span<const PlayerRuntime> players);
/** Each seat's exit and wait, as an imported level answers for it. */
std::array<std::optional<ImportedExit>, HudView::kSeats>
importedExitsOf(const ImportedLevel& level);
/** The pad an imported exit shows, named by its native code; none for an exit naming none
 * (no code, or code 0). */
std::optional<HudPad> importedPadOf(const ImportedExit& exit);
/** The bound meters' readings, filtered and capped as BossMeters::bind filters them; the count. */
usize hudBossReadingsOf(std::span<const HealthMeterReading> readings,
                        std::array<HudBossReading, BossMeters::kCapacity>& out);
/** Where Dark Legacy's help sits this frame: its anchor projected, else the canvas centre; none
 * while no message is up. */
std::optional<HudHelp> hudHelpOf(const HelpMessages& help, std::optional<Vec3> anchor,
                                 std::span<const PlayerRuntime> players, const Mat4& clip,
                                 const Mat4& canvasProjection, f32 width, f32 height);

} // namespace gdl::game
