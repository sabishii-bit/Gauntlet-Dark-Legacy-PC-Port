#include "game/app/CommandLine.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

CommandLineResult fail(ApplicationDesc desc, std::string message) {
    CommandLineResult result;
    result.action = CommandLineAction::Fail;
    result.desc = std::move(desc);
    result.message = std::move(message);
    return result;
}

} // namespace

CommandLineResult parseCommandLine(std::span<const std::string_view> args, ApplicationDesc defaults,
                                   GameOptions defaultOptions) {
    CommandLineResult result;
    result.desc = std::move(defaults);
    result.options = std::move(defaultOptions);
    ApplicationDesc& desc = result.desc;
    if (result.options.unpackedDirectory.empty()) {
        result.options.unpackedDirectory = desc.assetDirectory;
    }

    for (usize i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool hasValue = i + 1 < args.size();

        if (arg == "--assets") {
            if (!hasValue) {
                return fail(std::move(desc), "--assets requires a directory");
            }
            desc.assetDirectory = args[++i];
            result.options.unpackedDirectory = desc.assetDirectory;
        } else if (arg == "--movie") {
            if (!hasValue) {
                return fail(std::move(desc), "--movie requires a movie name");
            }
            result.options.playMovie = args[++i];
        } else if (arg == "--unpacked") {
            return fail(std::move(desc),
                        "--unpacked is retired; use --assets with the original Gauntlet directory");
        } else if (arg == "--data") {
            if (!hasValue) {
                return fail(std::move(desc), "--data requires a directory");
            }
            result.options.dataDirectory = args[++i];
        } else if (arg == "--title") {
            result.options.startAtTitle = true;
        } else if (arg == "--demo") {
            result.options.startAtDemo = true;
        } else if (arg == "--screensaver") {
            result.options.previewScreensaver = true;
        } else if (arg == "--scenario") {
            if (!hasValue) {
                return fail(std::move(desc), "--scenario requires a file");
            }
            result.options.scenario = args[++i];
        } else if (arg == "--no-vsync") {
            desc.vsync = false;
        } else if (arg == "--netplay-auto-start") {
            result.options.netplayAutoStart = true;
        } else if (arg == "--netplay-test" || arg == "--netplay-room" ||
                   arg == "--netplay-content" || arg == "--netplay-invite") {
            if (!hasValue || args[i + 1].empty()) {
                return fail(std::move(desc), std::format("{} requires a value", arg));
            }
            const auto value = args[++i];
            if (arg == "--netplay-test") {
                result.options.netplayTest = value;
            } else if (arg == "--netplay-room") {
                result.options.netplayRoom = value;
            } else if (arg == "--netplay-invite") {
                result.options.netplayInvite = value;
            } else {
                result.options.netplayContent = value;
            }
        } else if (arg == "--validation") {
            desc.enableValidation = true;
        } else if (arg == "--no-validation") {
            desc.enableValidation = false;
        } else if (arg == "--frames") {
            if (!hasValue) {
                return fail(std::move(desc), "--frames requires a number");
            }
            const std::string_view value = args[++i];
            u64 frames = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), frames);
            if (error != std::errc{} || end != value.data() + value.size()) {
                return fail(std::move(desc), std::format("--frames: '{}' is not a number", value));
            }
            desc.maxFrames = frames;
        } else if (arg == "--version") {
            result.action = CommandLineAction::ShowVersion;
            return result;
        } else if (arg == "--help" || arg == "-h") {
            result.action = CommandLineAction::ShowHelp;
            return result;
        } else {
            return fail(std::move(desc), std::format("unknown argument '{}'", arg));
        }
    }

    const auto& online = result.options;
    if (!online.netplayTest.empty() || !online.netplayRoom.empty() ||
        !online.netplayContent.empty() || !online.netplayInvite.empty() ||
        online.netplayAutoStart) {
        if ((online.netplayTest != "local" && online.netplayTest != "internet") ||
            online.scenario.empty() || online.startAtTitle || online.startAtDemo ||
            online.previewScreensaver || !online.playMovie.empty() ||
            online.netplayContent.size() != 64 ||
            !std::ranges::all_of(
                online.netplayContent,
                [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            online.netplayRoom.empty() == online.netplayInvite.empty()) {
            return fail(std::move(desc),
                        "Online test requires local or internet, a scenario, a SHA-256 content "
                        "identity and either --netplay-invite (host) or --netplay-room (guest)");
        }
        desc.window.title = online.netplayRoom.empty() ? "Gauntlet Dark Legacy - Netplay Host"
                                                       : "Gauntlet Dark Legacy - Netplay Guest";
    }
    result.action = CommandLineAction::Run;
    return result;
}

const char* usageText() {
    return "gauntlet [options]\n"
           "  --assets <dir>     retail Gauntlet tree for gameplay, movies and streams\n"
           "  --movie <name>     play one VQ movie (e.g. opening) and quit\n"
           "  --data <dir>       configuration and text directory (default beside executable)\n"
           "  --title            start at the title screen instead of the intro movies\n"
           "  --demo             preview a level flyby without the title wait\n"
           "  --screensaver      preview the idle weapons; any input exits\n"
           "  --scenario <file>  open the tower straight into the start the file describes\n"
           "  --netplay-test <local|internet> experimental scene test (requires --scenario)\n"
           "  --netplay-room <file> join using a private invitation file\n"
           "  --netplay-invite <file> host and write a private invitation file\n"
           "  --netplay-content <sha256> asset identity supplied by the test launcher\n"
           "  --netplay-auto-start start the paired-window test once both machines are ready\n"
           "  --no-vsync         present as fast as possible\n"
           "  --validation       force the Vulkan validation layer on\n"
           "  --no-validation    force it off (default on in Debug builds)\n"
           "  --frames <n>       quit after n frames (smoke testing)\n"
           "  --version          print the build version and exit\n"
           "  --help             this text\n"
           "Defaults use Gauntlet/ beside the executable, then the configured developer tree.\n"
           "Keep the original carddemo/ alongside Gauntlet/. No export step is required.";
}

} // namespace gdl::game
