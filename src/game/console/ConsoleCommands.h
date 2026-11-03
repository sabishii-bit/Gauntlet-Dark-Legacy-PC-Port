#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Expected.h"
#include "engine/math/Math.h"

#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

/**
 * The developer console's goto words as far as they need no scene: reading where `goto` is
 * asked to take the party, its usage, and naming the Legends maps and their platforms. Scene
 * and application callers wire them to the party and level in play. A usage is a few lines,
 * its forms in a column with what each does beside them.
 */
class ConsoleCommands {
public:
    using Arguments = std::span<const std::string>;

    /** Whether a word names a level the way the catalogue does ("E2", "TOWER"). */
    static bool looksLikeMap(std::string_view word);

    /** Where `goto` sends the party: a level by name, a point, a point in a level, a Legends
     * map (walked, or flown through as a preview), or back out of one. */
    struct Destination {
        std::optional<std::string> level; ///< as the catalogue names it, "G1"; "TOWER" the tower
        std::optional<Vec3> position;
        std::optional<std::string> legends;      ///< a Legends map, as its folder names it
        std::optional<LegendsPlatform> platform; ///< the map's platform, when named
        bool preview = false; ///< fly through the Legends map rather than walk it
        bool back = false;    ///< leave the Legends map for the level left for it
        /** A Legends map or the way back from one: this machine's alone, never the shared
         * game's. */
        bool offlineOnly() const { return back || legends.has_value(); }
        bool operator==(const Destination&) const = default;
    };
    static std::string gotoUsage();
    /** Reads `goto`'s arguments; the usage when they make no sense. */
    static gdl::Expected<Destination, std::string> parseGoto(Arguments arguments);
    /** The platform a word names: n64 or dreamcast, in any case. */
    static std::optional<LegendsPlatform> platformOf(std::string_view word);
    static std::string_view platformName(LegendsPlatform platform);

    /** A platform's installed Legends maps on one line, as `goto` takes them; none when
     * none are installed. */
    static std::vector<std::string> legendsMapLines(LegendsPlatform platform,
                                                    std::span<const std::string> names);
};

} // namespace gdl::game
