#pragma once
#include <optional>
#include <string>

namespace gdl::game {

/** Where a shared command's checked line goes to be run by the game in play; it says what the
 * console shows at once, if anything. */
class ConsoleActionSink {
public:
    virtual std::optional<std::string> submit(std::string line) = 0;

protected:
    ~ConsoleActionSink() = default;
};

} // namespace gdl::game
