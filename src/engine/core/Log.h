#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace gdl::log {

enum class Level : std::uint8_t { Trace, Info, Warn, Error };

/** Writes one formatted line to the console and, on Windows, the debugger output. */
void write(Level level, std::string_view message);

template <class... Args> void trace(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args> void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args> void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args> void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace gdl::log
