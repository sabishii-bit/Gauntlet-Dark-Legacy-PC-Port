#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"

namespace gdl::game {

class DevConsole;

/** The commands a Legends map's preview lets the console run while it holds the scene. */
inline constexpr std::array<std::string_view, 2> kLegendsPreviewWords{"help", "goto"};

/** The platform a word names: n64, or dreamcast (dc), in any case. */
std::optional<LegendsPlatform> legendsPlatformOf(std::string_view word);
/** The platform's name as the console and the level's realm show it: "N64" or "Dreamcast". */
std::string_view legendsPlatformName(LegendsPlatform platform);
/** A platform's installed Legends maps on one line, as `goto` takes them; none when none are
 * installed. */
std::vector<std::string> legendsMapLines(LegendsPlatform platform,
                                         std::span<const std::string> names);

/** What the Legends maps' console words read and do of the application. */
struct LegendsConsoleAccess {
    /** Whether a Legends map is previewed or walked now. */
    std::function<bool()> visiting;
    /** Whether either platform has a map installed, the disk walked again when `rescan`
     * asks; nothing is copied. */
    std::function<bool(bool rescan)> installed;
    /** The names of a platform's installed maps, as last found. */
    std::function<std::vector<std::string>(LegendsPlatform platform)> mapNames;
    /** The folder of the map named `name`: on `platform`, or on whichever has it. */
    std::function<std::optional<std::pair<LegendsPlatform, std::filesystem::path>>(
        std::string_view name, std::optional<LegendsPlatform> platform)>
        find;
    /** Opens the map in `directory`, walked when `walk`, else flown through as a preview. */
    std::function<void(DevConsole& console, LegendsPlatform platform,
                       const std::filesystem::path& directory, bool walk)>
        open;
    /** Leaves a Legends map or its preview for the level left for it. */
    std::function<void(DevConsole& console)> close;
    /** Whether offline play walks a native Legends world now. */
    std::function<bool()> walkingNative;
    /** Opens the native world's own tower to the party. */
    std::function<void(DevConsole& console)> unlockTower;
};

/**
 * Registers the console words the Legends maps answer offline with `host`: goto's maps and the
 * way back from one, shown in help, goto's usage and its words only while a map is installed or
 * visited; unlock_tower in a native world walked; and list maps' Legends lines in `listings`.
 */
void registerLegendsConsoleWords(ConsoleHost& host, ConsoleListings& listings,
                                 LegendsConsoleAccess access);

} // namespace gdl::game
