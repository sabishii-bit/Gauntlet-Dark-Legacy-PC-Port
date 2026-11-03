#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::game {

/** A command the wire admits as a shared console line: context-free, the same on every peer. */
struct SharedConsoleWord {
    std::string_view name;      ///< in lower case, as the console registers it
    std::string_view localForm; ///< a first argument that keeps the words on the asking machine
                                ///< ("info"), or empty
};

/** The shared console's commands, in the order the console registers them. */
inline constexpr std::array<SharedConsoleWord, 8> kSharedConsoleWords{{{"goto", {}},
                                                                       {"unlock_tower", {}},
                                                                       {"get", "info"},
                                                                       {"spawn", {}},
                                                                       {"suicide", {}},
                                                                       {"notarget", {}},
                                                                       {"god", {}},
                                                                       {"noclip", {}}}};

/** The shared command named `lowered`, a name already in lower case; null for any other. */
constexpr const SharedConsoleWord* findSharedConsoleWord(std::string_view lowered) {
    for (const SharedConsoleWord& word : kSharedConsoleWords) {
        if (word.name == lowered) {
            return &word;
        }
    }
    return nullptr;
}

/** The shared console grammar: a line's words, split on spaces only, none empty. Every console
 * reads its lines this way. */
std::vector<std::string> splitConsoleWords(std::string_view line);

} // namespace gdl::game
