#pragma once
#include <string>
#include <vector>

namespace gdl::game {

/** Words a command or form offers for the word being typed: its grammar's words, which are
 * offered first, then the names it knows. */
struct ConsoleWords {
    std::vector<std::string> keywords; ///< such as tower, back or preview
    std::vector<std::string> names;    ///< such as levels or Legends maps
};

} // namespace gdl::game
