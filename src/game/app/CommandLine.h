#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "engine/app/Application.h"
#include "engine/core/Types.h"

namespace gdl::game {

enum class CommandLineAction : u8 { Run, ShowHelp, Fail };

struct GameOptions {
    std::string playMovie; ///< play this VQ movie (name without extension) and quit
    std::filesystem::path unpackedDirectory; ///< output of gdlunpack (PNG images, JSON manifests)
    std::filesystem::path dataDirectory;     ///< shipped configuration and text (the data/ tree)
    bool startAtTitle = false;               ///< skip the intro movies and open the title screen
    std::filesystem::path scenario;          ///< a described start to open straight into
};

struct CommandLineResult {
    CommandLineAction action = CommandLineAction::Run;
    ApplicationDesc desc;
    GameOptions options;
    std::string message; ///< error text when action is Fail
};

/** Applies the process arguments (without argv[0]) on top of `defaults`. */
CommandLineResult parseCommandLine(std::span<const std::string_view> args, ApplicationDesc defaults,
                                   GameOptions defaultOptions = {});

const char* usageText();

} // namespace gdl::game
