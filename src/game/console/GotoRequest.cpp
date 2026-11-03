#include "game/console/GotoRequest.h"

#include <algorithm>
#include <vector>

#include "engine/core/Strings.h"

#include "game/console/ConsoleArguments.h"
#include "game/console/ConsoleUsage.h"

namespace gdl::game {

namespace {
using Arguments = ConsoleCommand::Arguments;

constexpr std::string_view kPreviewWord = "preview";

std::optional<Vec3> point(Arguments arguments) {
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

/** The platform a word names, in any case: "n64", or "dreamcast" for dreamcast or dc. */
std::optional<std::string> platformWord(std::string_view word) {
    const std::string name = toLowerAscii(word);
    if (name == "n64") {
        return "n64";
    }
    if (name == "dreamcast" || name == "dc") {
        return "dreamcast";
    }
    return std::nullopt;
}
} // namespace

bool looksLikeMap(std::string_view word) {
    const std::string name = upperArgument(word);
    if (name == "TOWER") {
        return true;
    }
    if (name.size() < 2 || name.size() > 3 || name.front() < 'A' || name.front() > 'Z') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}

std::string sharedGotoUsage() {
    std::vector<std::pair<std::string_view, std::string_view>> rows(GotoRequest::kUsageRows.begin(),
                                                                    GotoRequest::kUsageRows.end());
    rows.insert(rows.end(), GotoRequest::kLegendsUsageRows.begin(),
                GotoRequest::kLegendsUsageRows.end());
    constexpr std::array<std::string_view, 1> kNotes{GotoRequest::kLegendsUsageNote};
    return consoleUsage(rows, kNotes);
}

gdl::Expected<GotoRequest, std::string> parseGoto(Arguments arguments) {
    // A Legends map: by its platform, or by name alone, flown through with "preview".
    Arguments rest = arguments;
    GotoRequest legends;
    if (!rest.empty()) {
        legends.platform = platformWord(rest[0]);
        if (legends.platform.has_value()) {
            rest = rest.subspan(1);
        }
    }
    if (!rest.empty() && rest.size() <= 2 && !integerArgument(rest[0]).has_value() &&
        (rest.size() == 1 || toLowerAscii(rest[1]) == kPreviewWord)) {
        const std::string word = toLowerAscii(rest[0]);
        if (word == "back" || word == "return") {
            if (rest.size() == 1 && !legends.platform.has_value()) {
                return GotoRequest{.back = true};
            }
            return gdl::Unexpected(sharedGotoUsage());
        }
        if (legends.platform.has_value() || !looksLikeMap(word)) {
            legends.legends = std::string(rest[0]);
            legends.preview = rest.size() == 2;
            return legends;
        }
    }
    if (legends.platform.has_value()) {
        return gdl::Unexpected(sharedGotoUsage());
    }
    if (arguments.size() == 1) {
        return GotoRequest{.level = upperArgument(arguments[0])};
    }
    if (arguments.size() == 3) {
        if (const auto at = point(arguments)) {
            return GotoRequest{.position = at};
        }
        return gdl::Unexpected(sharedGotoUsage());
    }
    if (arguments.size() == 4) {
        if (const auto at = point(arguments.subspan(1))) {
            return GotoRequest{.level = upperArgument(arguments[0]), .position = at};
        }
    }
    return gdl::Unexpected(sharedGotoUsage());
}

} // namespace gdl::game
