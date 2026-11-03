#include "game/console/ConsoleCommands.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "engine/core/Strings.h"

#include "game/console/ConsoleArguments.h"
#include "game/console/ConsoleUsage.h"

namespace gdl::game {

namespace {
constexpr std::string_view kPreviewWord = "preview";

std::optional<Vec3> point(ConsoleCommands::Arguments arguments) {
    if (arguments.size() != 3) {
        return std::nullopt;
    }
    const auto x = numberArgument(arguments[0]);
    const auto y = numberArgument(arguments[1]);
    const auto z = numberArgument(arguments[2]);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{*x, *y, *z};
}
} // namespace

std::string ConsoleCommands::gotoUsage() {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kRows{
        {{"goto <map> [x y z]", "travels to a level, standing at a point when given"},
         {"goto x y z", "moves the party to that point here"},
         {"goto <legends map> [preview]", "walks an installed Legends map, or flies through it"},
         {"goto n64|dreamcast <map>", "the same, naming the map's platform"},
         {"goto back", "returns from a Legends map to the level left for it"}}};
    constexpr std::array<std::string_view, 1> kNotes{
        "list maps names the levels and the Legends maps installed."};
    return consoleUsage(kRows, kNotes);
}

std::optional<LegendsPlatform> ConsoleCommands::platformOf(std::string_view word) {
    const std::string name = toLowerAscii(word);
    if (name == "n64") {
        return LegendsPlatform::N64;
    }
    if (name == "dreamcast" || name == "dc") {
        return LegendsPlatform::Dreamcast;
    }
    return std::nullopt;
}

std::string_view ConsoleCommands::platformName(LegendsPlatform platform) {
    return platform == LegendsPlatform::N64 ? "N64" : "Dreamcast";
}

bool ConsoleCommands::looksLikeMap(std::string_view word) {
    const std::string name = upperArgument(word);
    if (name == "TOWER") {
        return true;
    }
    if (name.size() < 2 || name.size() > 3 || name.front() < 'A' || name.front() > 'Z') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}

gdl::Expected<ConsoleCommands::Destination, std::string>
ConsoleCommands::parseGoto(Arguments arguments) {
    // A Legends map: by its platform, or by name alone, flown through with "preview".
    Arguments rest = arguments;
    Destination legends;
    if (!rest.empty()) {
        legends.platform = platformOf(rest[0]);
        if (legends.platform.has_value()) {
            rest = rest.subspan(1);
        }
    }
    if (!rest.empty() && rest.size() <= 2 && !integerArgument(rest[0]).has_value() &&
        (rest.size() == 1 || toLowerAscii(rest[1]) == kPreviewWord)) {
        const std::string word = toLowerAscii(rest[0]);
        if (word == "back" || word == "return") {
            if (rest.size() == 1 && !legends.platform.has_value()) {
                return Destination{.back = true};
            }
            return gdl::Unexpected(gotoUsage());
        }
        if (legends.platform.has_value() || !looksLikeMap(word)) {
            legends.legends = std::string(rest[0]);
            legends.preview = rest.size() == 2;
            return legends;
        }
    }
    if (legends.platform.has_value()) {
        return gdl::Unexpected(gotoUsage());
    }
    if (arguments.size() == 1) {
        return Destination{.level = upperArgument(arguments[0])};
    }
    if (arguments.size() == 3) {
        if (const auto at = point(arguments)) {
            return Destination{.position = at};
        }
        return gdl::Unexpected(gotoUsage());
    }
    if (arguments.size() == 4) {
        if (const auto at = point(arguments.subspan(1))) {
            return Destination{.level = upperArgument(arguments[0]), .position = at};
        }
    }
    return gdl::Unexpected(gotoUsage());
}

std::vector<std::string> ConsoleCommands::legendsMapLines(LegendsPlatform platform,
                                                          std::span<const std::string> names) {
    if (names.empty()) {
        return {};
    }
    std::string joined;
    for (const std::string& name : names) {
        joined += joined.empty() ? "" : ", ";
        joined += name;
    }
    return {std::format("{} Legends maps (goto <name>): {}", platformName(platform), joined)};
}

} // namespace gdl::game
