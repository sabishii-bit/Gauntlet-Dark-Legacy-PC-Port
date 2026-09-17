#include "game/app/CommandLine.h"

#include <charconv>
#include <format>
#include <system_error>
#include <utility>

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

    for (usize i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool hasValue = i + 1 < args.size();

        if (arg == "--assets") {
            if (!hasValue) {
                return fail(std::move(desc), "--assets requires a directory");
            }
            desc.assetDirectory = args[++i];
        } else if (arg == "--movie") {
            if (!hasValue) {
                return fail(std::move(desc), "--movie requires a movie name");
            }
            result.options.playMovie = args[++i];
        } else if (arg == "--unpacked") {
            if (!hasValue) {
                return fail(std::move(desc), "--unpacked requires a directory");
            }
            result.options.unpackedDirectory = args[++i];
        } else if (arg == "--data") {
            if (!hasValue) {
                return fail(std::move(desc), "--data requires a directory");
            }
            result.options.dataDirectory = args[++i];
        } else if (arg == "--title") {
            result.options.startAtTitle = true;
        } else if (arg == "--no-vsync") {
            desc.vsync = false;
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
        } else if (arg == "--help" || arg == "-h") {
            result.action = CommandLineAction::ShowHelp;
            return result;
        } else {
            return fail(std::move(desc), std::format("unknown argument '{}'", arg));
        }
    }

    result.action = CommandLineAction::Run;
    return result;
}

const char* usageText() {
    return "gauntlet [options]\n"
           "  --assets <dir>     game asset directory (the GUNE5D/Gauntlet tree)\n"
           "  --movie <name>     play one VQ movie (e.g. opening) and quit\n"
           "  --unpacked <dir>   gdlunpack output directory (default assets/unpacked)\n"
           "  --data <dir>       configuration and text directory (default data/)\n"
           "  --title            start at the title screen instead of the intro movies\n"
           "  --no-vsync         present as fast as possible\n"
           "  --validation       force the Vulkan validation layer on\n"
           "  --no-validation    force it off (default on in Debug builds)\n"
           "  --frames <n>       quit after n frames (smoke testing)\n"
           "  --help             this text";
}

} // namespace gdl::game
