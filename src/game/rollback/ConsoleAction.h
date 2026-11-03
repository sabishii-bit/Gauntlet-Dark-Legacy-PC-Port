#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/rollback/SharedConsoleWords.h"

namespace gdl::game {

/** A bounded console mutation admitted by the host, then recorded as a one-shot input.
 * The seat is the player the command speaks for, not the input seat carrying it.
 * Parsing command arguments and applying them belongs to the native scene. */
struct ConsoleAction {
    static constexpr usize kMaxLine = 160;
    u8 seat = 0;
    std::string line;

    /** The words a console line may carry, whatever its length: a seat, printable characters
     * and a shared verb, never one of its local forms (get info), in any case. It splits the
     * line on spaces itself, as splitConsoleWords does, and compares its words in lower case,
     * so a decoder built without the game library judges alike. */
    bool admitted() const {
        if (seat >= 4 || line.empty() ||
            !std::ranges::all_of(line, [](unsigned char c) { return c >= 32 && c <= 126; })) {
            return false;
        }
        std::string_view words{line};
        const auto next = [&]() {
            const auto begin = words.find_first_not_of(' ');
            if (begin == std::string_view::npos) {
                words = {};
                return std::string{};
            }
            words.remove_prefix(begin);
            const auto end = words.find(' ');
            std::string word{words.substr(0, end)};
            words = end == std::string_view::npos ? std::string_view{} : words.substr(end);
            for (char& c : word) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return word;
        };
        const SharedConsoleWord* verb = findSharedConsoleWord(next());
        return verb != nullptr && (verb->localForm.empty() || next() != verb->localForm);
    }
    /** What the wire carries: an admitted line of at most kMaxLine characters. */
    bool valid() const { return line.size() <= kMaxLine && admitted(); }
    bool operator==(const ConsoleAction&) const = default;
};

} // namespace gdl::game
