#pragma once

#include <span>
#include <string>
#include <string_view>

#include "engine/app/Application.h"
#include "engine/core/Types.h"

namespace gdl::game {

enum class CommandLineAction : u8 { Run, ShowHelp, Fail };

struct GameOptions {
    std::string playMovie; ///< play this VQ movie (name without extension) and quit
};

struct CommandLineResult {
    CommandLineAction action = CommandLineAction::Run;
    ApplicationDesc desc;
    GameOptions options;
    std::string message; ///< error text when action is Fail
};

/** Applies the process arguments (without argv[0]) on top of `defaults`. */
CommandLineResult parseCommandLine(std::span<const std::string_view> args,
                                   ApplicationDesc defaults);

const char* usageText();

} // namespace gdl::game
