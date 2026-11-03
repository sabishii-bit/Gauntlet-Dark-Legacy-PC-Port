#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Expected.h"
#include "engine/math/Math.h"

#include "game/console/ConsoleCommand.h"

namespace gdl::game {

/** Where `goto` is asked to take the party: a level by name, a point, a point in a level, a
 * Legends map (walked, or flown through as a preview), or back out of one. */
struct GotoRequest {
    /** goto's own forms, each with what it does, in the order its usage lists them. */
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kUsageRows{
        {{"goto <map> [x y z]", "travels to a level, standing at a point when given"},
         {"goto x y z", "moves the party to that point here"}}};
    /** The forms that name a Legends map, as the usage lists them after goto's own. */
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kLegendsUsageRows{
        {{"goto <legends map> [preview]", "walks an installed Legends map, or flies through it"},
         {"goto n64|dreamcast <map>", "the same, naming the map's platform"},
         {"goto back", "returns from a Legends map to the level left for it"}}};
    /** What the usage says under the forms when they name the Legends maps. */
    static constexpr std::string_view kLegendsUsageNote =
        "list maps names the levels and the Legends maps installed.";

    std::optional<std::string> level; ///< as the catalogue names it, "G1"; "TOWER" the tower
    std::optional<Vec3> position;
    std::optional<std::string> legends;  ///< a Legends map, as typed: it names a folder
    std::optional<std::string> platform; ///< the map's platform when named, "n64" or "dreamcast"
    bool preview = false;                ///< fly through the Legends map rather than walk it
    bool back = false;                   ///< leave the Legends map for the level left for it
    /** A Legends map or the way back from one: this machine's alone, never the shared game's. */
    bool offlineOnly() const { return back || legends.has_value(); }
    bool operator==(const GotoRequest&) const = default;
};

/** Whether a word names a level the way the catalogue does ("E2", "TOWER"). */
bool looksLikeMap(std::string_view word);
/** Every form and the note: what the shared game answers a goto it cannot read. */
std::string sharedGotoUsage();
/** Reads `goto`'s arguments: a level, a point, the tower, or the words that name a Legends map
 * or the way back from one; sharedGotoUsage() when they make no sense. */
gdl::Expected<GotoRequest, std::string> parseGoto(ConsoleCommand::Arguments arguments);

} // namespace gdl::game
