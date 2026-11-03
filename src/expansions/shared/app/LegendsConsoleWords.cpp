#include "expansions/shared/app/LegendsConsoleWords.h"

#include <format>
#include <memory>

#include "engine/core/Assert.h"
#include "engine/core/Strings.h"
#include "engine/platform/Paths.h"

#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleWords.h"
#include "game/console/DevConsole.h"
#include "game/console/GotoRequest.h"
#include "game/scenes/SharedConsoleVerbs.h"

namespace gdl::game {

namespace {
constexpr std::array<LegendsPlatform, 2> kPlatforms{LegendsPlatform::N64,
                                                    LegendsPlatform::Dreamcast};

/**
 * goto's Legends words: every word goto reads as a Legends map or the way back from one, claimed
 * whatever is installed, so their replies never change. Help, the usage and the words show them
 * only while a map is installed or visited. Online the console refuses them in the shared verb's
 * words.
 */
class LegendsGotoForm final : public ConsoleForm {
public:
    explicit LegendsGotoForm(LegendsConsoleAccess access) : m_access(std::move(access)) {}

    bool claims(Arguments arguments) const override {
        const auto request = parseGoto(arguments);
        return request.has_value() && request->offlineOnly();
    }
    ConsoleReach reach() const override { return ConsoleReach::OfflineOnly; }
    void run(DevConsole& console, Arguments arguments) override {
        const auto request = parseGoto(arguments);
        GDL_VERIFY(request.has_value() && request->offlineOnly(),
                   "goto's Legends form claims its own words alone");
        if (request->back) {
            m_access.close(console);
            return;
        }
        const std::string& name = *request->legends;
        const std::optional<LegendsPlatform> platform =
            request->platform.has_value() ? legendsPlatformOf(*request->platform) : std::nullopt;
        const bool path = name.find_first_of("/\\") != std::string::npos;
        if (path && !platform.has_value()) {
            console.print("Name the platform with a path: goto n64|dreamcast <path>");
            return;
        }
        const auto found = path ? std::optional{std::pair{*platform, paths::fromUtf8(name)}}
                                : m_access.find(name, platform);
        if (!found.has_value()) {
            // Neither a level nor an installed Legends map: list maps names both.
            console.print(
                std::format("No level or Legends map named {}; list maps names them", name));
            return;
        }
        m_access.open(console, found->first, found->second, !request->preview);
    }
    std::string_view summaryForm() const override { return "<legends map> [preview]"; }
    std::vector<std::pair<std::string, std::string>> usageRows() const override {
        std::vector<std::pair<std::string, std::string>> rows;
        for (const auto& [form, what] : GotoRequest::kLegendsUsageRows) {
            rows.emplace_back(form, what);
        }
        return rows;
    }
    std::string_view usageNote() const override { return GotoRequest::kLegendsUsageNote; }
    std::string_view refusal() const override {
        const SharedConsoleVerb* verb = sharedConsoleVerb("goto");
        return verb != nullptr ? verb->offlineOnlyRefusal() : std::string_view{};
    }
    /** The way back and the maps installed for the first word; preview after any first. */
    ConsoleWords words(Arguments arguments) const override {
        if (arguments.size() > 1) {
            return arguments.size() == 2 ? ConsoleWords{.keywords = {"preview"}, .names = {}}
                                         : ConsoleWords{};
        }
        ConsoleWords offered{.keywords = {"back"}, .names = {}};
        for (const LegendsPlatform platform : kPlatforms) {
            const std::vector<std::string> names = m_access.mapNames(platform);
            offered.names.insert(offered.names.end(), names.begin(), names.end());
        }
        return offered;
    }
    bool listed() const override { return m_access.visiting() || m_access.installed(false); }

private:
    LegendsConsoleAccess m_access;
};
} // namespace

std::optional<LegendsPlatform> legendsPlatformOf(std::string_view word) {
    const std::string name = toLowerAscii(word);
    if (name == "n64") {
        return LegendsPlatform::N64;
    }
    if (name == "dreamcast" || name == "dc") {
        return LegendsPlatform::Dreamcast;
    }
    return std::nullopt;
}

std::string_view legendsPlatformName(LegendsPlatform platform) {
    return platform == LegendsPlatform::N64 ? "N64" : "Dreamcast";
}

std::vector<std::string> legendsMapLines(LegendsPlatform platform,
                                         std::span<const std::string> names) {
    if (names.empty()) {
        return {};
    }
    std::string joined;
    for (const std::string& name : names) {
        joined += joined.empty() ? "" : ", ";
        joined += name;
    }
    return {
        std::format("{} Legends maps (goto <name>): {}", legendsPlatformName(platform), joined)};
}

void registerLegendsConsoleWords(ConsoleHost& host, ConsoleListings& listings,
                                 LegendsConsoleAccess access) {
    // unlock_tower in a native world walked offline opens that world's own tower; elsewhere the
    // shared verb answers it.
    auto unlockTower = std::make_unique<LambdaForm>(
        ConsoleReach::OfflineOnly, std::string{},
        [walkingNative = access.walkingNative](ConsoleForm::Arguments /*arguments*/) {
            return walkingNative();
        },
        [unlock = access.unlockTower](DevConsole& console, ConsoleForm::Arguments /*arguments*/) {
            unlock(console);
        });
    // list maps names the Legends maps installed after the levels, each platform's on a line,
    // found afresh; nothing is said of a platform without any.
    auto maps = std::make_unique<LambdaListing>(
        "maps", std::nullopt,
        [installed = access.installed,
         mapNames = access.mapNames](ConsoleListing::Arguments /*rest*/) {
            installed(true);
            std::vector<std::string> lines;
            for (const LegendsPlatform platform : kPlatforms) {
                const std::vector<std::string> names = mapNames(platform);
                const std::vector<std::string> platformLines = legendsMapLines(platform, names);
                lines.insert(lines.end(), platformLines.begin(), platformLines.end());
            }
            return lines;
        });
    host.addForm("goto", std::make_unique<LegendsGotoForm>(std::move(access)));
    host.addForm("unlock_tower", std::move(unlockTower));
    listings.add(std::move(maps));
}

} // namespace gdl::game
