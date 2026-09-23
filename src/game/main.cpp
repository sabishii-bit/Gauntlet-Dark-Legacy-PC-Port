#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/app/Application.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/app/CommandLine.h"
#include "game/app/Gauntlet.h"
#include "game/config/GameConfig.h"

#ifndef GDL_DEFAULT_ASSET_DIR
#define GDL_DEFAULT_ASSET_DIR ""
#endif
#ifndef GDL_DEFAULT_UNPACKED_DIR
#define GDL_DEFAULT_UNPACKED_DIR ""
#endif
#ifndef GDL_DEFAULT_DATA_DIR
#define GDL_DEFAULT_DATA_DIR ""
#endif

namespace {

int runGauntlet(std::span<char*> rawArgs) {
    std::vector<std::string_view> args;
    for (const char* arg : rawArgs.subspan(rawArgs.empty() ? 0 : 1)) {
        args.emplace_back(arg);
    }

    gdl::ApplicationDesc defaults;
    defaults.window.title = "Gauntlet Dark Legacy";
    defaults.assetDirectory = GDL_DEFAULT_ASSET_DIR;
    defaults.enableValidation = GDL_DEBUG != 0;

    gdl::game::GameOptions defaultOptions;
    defaultOptions.unpackedDirectory = GDL_DEFAULT_UNPACKED_DIR;
    defaultOptions.dataDirectory = GDL_DEFAULT_DATA_DIR;

    gdl::game::CommandLineResult parsed =
        gdl::game::parseCommandLine(args, std::move(defaults), std::move(defaultOptions));
    switch (parsed.action) {
    case gdl::game::CommandLineAction::ShowHelp: std::puts(gdl::game::usageText()); return 0;
    case gdl::game::CommandLineAction::Fail:
        gdl::log::error("{}", parsed.message);
        std::puts(gdl::game::usageText());
        return 2;
    case gdl::game::CommandLineAction::Run: break;
    }

    // Settings: the shipped defaults, then the player's own file, then the command line.
    gdl::game::GameConfig config;
    config.loadFile(parsed.options.dataDirectory / "config.json");
    const std::filesystem::path userSettings = gdl::game::GameConfig::userSettingsPath();
    if (std::filesystem::exists(userSettings)) {
        config.loadFile(userSettings);
    }
    const bool vsyncFromCommandLine = !parsed.desc.vsync;
    parsed.desc.window.width = config.display.windowWidth;
    parsed.desc.window.height = config.display.windowHeight;
    parsed.desc.vsync = config.display.vsync && !vsyncFromCommandLine;
    parsed.desc.maxFrameRate = config.display.maxFrameRate;

    gdl::game::Gauntlet game(std::move(parsed.desc), std::move(parsed.options), std::move(config));
    return game.run();
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return runGauntlet(std::span<char*>(argv, static_cast<gdl::usize>(argc)));
    } catch (const std::exception& e) {
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    } catch (...) {
        std::fputs("unhandled exception\n", stderr);
        return 1;
    }
}
