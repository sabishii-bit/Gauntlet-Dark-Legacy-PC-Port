#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <algorithm>
#include <bit>
#include <cwchar>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// shellapi.h takes its types from windows.h, so the order stands.
#include <windows.h> // IWYU pragma: keep

#include <shellapi.h>
// clang-format on
#endif

#include "engine/app/Application.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/Paths.h"

#include "game/app/AssetRoots.h"
#include "game/app/CommandLine.h"
#include "game/app/Gauntlet.h"
#include "game/app/NativeOnlineIdentity.h"
#include "game/config/GameConfig.h"
#include "game/mechanics/MechanicCatalog.h"

#ifndef GDL_DEFAULT_ASSET_DIR
#define GDL_DEFAULT_ASSET_DIR ""
#endif
#ifndef GDL_DEFAULT_DATA_DIR
#define GDL_DEFAULT_DATA_DIR ""
#endif

namespace {

#ifdef _WIN32
/** The command line as UTF-8, whatever the system code page: a path beyond that page
 * reaches the game whole. */
std::vector<std::string> utf8Arguments(std::span<char*> fallback) {
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return {fallback.begin(), fallback.end()};
    }
    const std::span<wchar_t*> arguments(wide, static_cast<gdl::usize>(std::max(count, 0)));
    std::vector<std::string> args;
    args.reserve(arguments.size());
    for (const wchar_t* argument : arguments) {
        const auto wideLength = static_cast<int>(std::wcslen(argument));
        const int length =
            WideCharToMultiByte(CP_UTF8, 0, argument, wideLength, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<gdl::usize>(std::max(length, 0)), '\0');
        if (length > 0) {
            WideCharToMultiByte(CP_UTF8, 0, argument, wideLength, arg.data(), length, nullptr,
                                nullptr);
        }
        args.push_back(std::move(arg));
    }
    LocalFree(std::bit_cast<HLOCAL>(wide)); // the same handle, without a pointer conversion
    return args;
}
#endif

int runGauntlet(std::span<const std::string> rawArgs) {
    std::vector<std::string_view> args;
    for (const std::string& arg : rawArgs.subspan(rawArgs.empty() ? 0 : 1)) {
        args.emplace_back(arg);
    }

    const auto roots = gdl::game::resolveAssetRoots(gdl::paths::executableDirectory(),
                                                    GDL_DEFAULT_ASSET_DIR, GDL_DEFAULT_DATA_DIR);
    gdl::ApplicationDesc defaults;
    defaults.window.title = "Gauntlet Dark Legacy";
    defaults.assetDirectory = roots.assets;
    defaults.enableValidation = GDL_DEBUG != 0;

    gdl::game::GameOptions defaultOptions;
    defaultOptions.unpackedDirectory = roots.assets;
    defaultOptions.dataDirectory = roots.data;

    gdl::game::CommandLineResult parsed =
        gdl::game::parseCommandLine(args, std::move(defaults), std::move(defaultOptions));
    switch (parsed.action) {
    case gdl::game::CommandLineAction::ShowHelp: std::puts(gdl::game::usageText()); return 0;
    case gdl::game::CommandLineAction::ShowVersion: std::puts(GDL_VERSION); return 0;
    case gdl::game::CommandLineAction::Fail:
        gdl::log::error("{}", parsed.message);
        std::puts(gdl::game::usageText());
        return 2;
    case gdl::game::CommandLineAction::Run: break;
    }

    gdl::log::info("Gauntlet Dark Legacy {}", GDL_VERSION);

    // Settings: the shipped defaults, then the player's own file, then the command line.
    gdl::game::GameConfig config;
    config.loadFile(parsed.options.dataDirectory / "config.json");
    // The paired-window harness supplies an isolated configuration. Neither
    // window should inherit fullscreen mode or another installation's devices.
    if (parsed.options.netplayTest.empty()) {
        config.loadUserSettings();
    }
    const bool vsyncFromCommandLine = !parsed.desc.vsync;
    parsed.desc.window.width = config.display.windowWidth;
    parsed.desc.window.height = config.display.windowHeight;
    parsed.desc.window.mode = config.display.windowMode;
    parsed.desc.window.monitor = config.display.monitor;
    parsed.desc.renderer = config.display.renderer;
    parsed.desc.vsync = config.display.vsync && !vsyncFromCommandLine;
    parsed.desc.sampleCount = config.display.sampleCount;
    parsed.desc.textureFiltering = config.display.textureFiltering;
    parsed.desc.maxFrameRate = config.display.maxFrameRate;
    parsed.desc.updateRate = config.timing.tickRate;
    // Rooms can also be entered from the title. Keep the render worker alive across that
    // transition; offline-only builds retain their existing loop.
    parsed.desc.threadedPresentation = gdl::game::OnlineRun::available();

    // Every mechanic the build's mods carry, offered in the settings and to online rooms.
    gdl::game::MechanicCatalog mechanics;
    gdl::game::registerCompiledMechanics(mechanics);

    gdl::game::Gauntlet game(std::move(parsed.desc), std::move(parsed.options), std::move(config),
                             GDL_VERSION, std::move(mechanics));
    return game.run();
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::string numericError;
        if (!gdl::game::NativeOnlineIdentity::prepareProcessNumeric(&numericError)) {
            std::fputs(numericError.c_str(), stderr);
            std::fputs("\n", stderr);
            return 1;
        }
        const std::span<char*> raw(argv, static_cast<gdl::usize>(argc));
#ifdef _WIN32
        const std::vector<std::string> arguments = utf8Arguments(raw);
#else
        const std::vector<std::string> arguments(raw.begin(), raw.end());
#endif
        return runGauntlet(arguments);
    } catch (const std::exception& e) {
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    } catch (...) {
        std::fputs("unhandled exception\n", stderr);
        return 1;
    }
}
