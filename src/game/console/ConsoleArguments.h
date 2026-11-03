#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/** A console command's word in upper case, as levels and kinds are named. */
std::string upperArgument(std::string_view argument);
/** The whole word read as an integer; nothing when any of it is not one. */
std::optional<s32> integerArgument(std::string_view argument);
/** The whole word read as a number; nothing when any of it is not one. */
std::optional<f32> numberArgument(std::string_view argument);

} // namespace gdl::game
