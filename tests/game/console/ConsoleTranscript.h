#pragma once
#include <array>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/console/DevConsole.h"

namespace gdl::test {

/** What `console` shows under its echo of `line` once it has run it, its lines joined by
 * breaks. When the scrollback has let the echo go, everything it keeps is the line's. */
inline std::string said(game::DevConsole& console, std::string_view line) {
    console.run(line);
    const std::string echo = std::format("{} {}", console.prompt(), line);
    const std::span<const std::string> output = console.output();
    usize first = 0;
    for (usize i = output.size(); i > 0; --i) {
        if (output[i - 1] == echo) {
            first = i;
            break;
        }
    }
    std::string shown;
    for (usize i = first; i < output.size(); ++i) {
        shown += shown.empty() ? "" : "\n";
        shown += output[i];
    }
    return shown;
}

/** What a fresh console shows of `lines` printed one after another, joined by breaks. */
inline std::string printed(std::span<const std::string> lines) {
    game::DevConsole console;
    for (const std::string& line : lines) {
        console.print(line);
    }
    std::string shown;
    for (const std::string& line : console.output()) {
        shown += shown.empty() ? "" : "\n";
        shown += line;
    }
    return shown;
}

/** What a fresh console shows of `text` printed. */
inline std::string shownAs(std::string_view text) {
    const std::vector<std::string> lines{std::string(text)};
    return printed(lines);
}

/** `parts` one after another, as one listing. */
inline std::vector<std::string> joinedLines(std::initializer_list<std::vector<std::string>> parts) {
    std::vector<std::string> all;
    for (const std::vector<std::string>& part : parts) {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

/** What `console` suggests once `typed` is typed into a fresh line, the console slid down by its
 * key first; it is closed again after, its line discarded. */
inline std::string suggestionAfter(game::DevConsole& console, std::string_view typed) {
    constexpr f64 kTick = 1.0 / 60.0;
    constexpr std::array<Key, 1> kToggle{Key::Grave};
    Input input;
    const auto poll = [&](bool toggleDown, std::string_view text) {
        input.beginPoll();
        input.setKey(Key::Grave, toggleDown);
        for (const char c : text) {
            input.addTypedChar(static_cast<u32>(static_cast<unsigned char>(c)));
        }
        console.update(kTick, input, kToggle);
    };
    console.close();
    poll(true, {});
    poll(false, {});
    for (usize i = 0; i < typed.size(); ++i) {
        poll(false, typed.substr(i, 1));
    }
    std::string rest = console.suggestion();
    console.close();
    return rest;
}

} // namespace gdl::test
