#include "game/console/ConsoleArguments.h"

#include <cctype>
#include <charconv>
#include <system_error>

namespace gdl::game {

std::string lowerArgument(std::string_view argument) {
    std::string result(argument);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string upperArgument(std::string_view argument) {
    std::string result(argument);
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

std::optional<s32> integerArgument(std::string_view argument) {
    s32 value = 0;
    const auto [end, error] =
        std::from_chars(argument.data(), argument.data() + argument.size(), value);
    return error == std::errc{} && end == argument.data() + argument.size() ? std::optional{value}
                                                                            : std::nullopt;
}

std::optional<f32> numberArgument(std::string_view argument) {
    f32 value = 0.0f;
    const auto [end, error] =
        std::from_chars(argument.data(), argument.data() + argument.size(), value);
    return error == std::errc{} && end == argument.data() + argument.size() ? std::optional{value}
                                                                            : std::nullopt;
}

} // namespace gdl::game
