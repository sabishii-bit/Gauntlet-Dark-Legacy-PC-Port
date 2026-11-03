#include "game/console/DevConsole.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <utility>

#include "engine/core/Log.h"

#include "game/rollback/SharedConsoleWords.h"

namespace gdl::game {

namespace {
constexpr std::string_view kFontFile = "fonts/font8x8.json";
constexpr std::string_view kTextureDirectory = "STATIC";
constexpr std::string_view kTextureName = "FONT8X8";
constexpr s32 kSpaceWidth = 8;
constexpr u32 kFirstTypable = 0x20;
constexpr u32 kLastTypable = 0x7E;

/** Where to break `text` for a line of `columns`: at the last space that fits, else hard. */
usize breakAt(std::string_view text, usize columns) {
    if (text.size() <= columns) {
        return text.size();
    }
    const usize space = text.rfind(' ', columns);
    return space == std::string_view::npos || space == 0 ? columns : space;
}
} // namespace

DevConsole::DevConsole() {
    addCommand("help", "lists the commands", ConsoleReach::Local,
               [](DevConsole& console, Arguments /*arguments*/) { console.help(); });
}

bool DevConsole::load(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    release();
    if (!m_font.load(unpackedRoot / kFontFile, kSpaceWidth) ||
        !m_textures.load(unpackedRoot / kTextureDirectory)) {
        return false;
    }
    const auto index = m_textures.find(kTextureName);
    if (!index.has_value()) {
        return false;
    }
    try {
        m_text.setFont(&m_font, &m_textures.texture(device, *index));
    } catch (const std::exception& e) {
        log::warn("Developer console: texture {}: {}", kTextureName, e.what());
        return false;
    }
    return true;
}

void DevConsole::release() {
    m_text.setFont(nullptr, nullptr);
    m_textures.releaseTextures();
}

void DevConsole::addCommand(std::unique_ptr<ConsoleCommand> command) {
    m_commands.add(std::move(command));
}

void DevConsole::addCommand(std::string name, std::string summary, ConsoleReach reach,
                            Handler handler, Completer completer) {
    m_commands.add(std::move(name), std::move(summary), reach, std::move(handler),
                   std::move(completer));
}

void DevConsole::addForm(std::string_view command, std::unique_ptr<ConsoleForm> form) {
    m_commands.addForm(command, std::move(form));
}

void DevConsole::print(std::string_view text) {
    usize start = 0;
    do {
        const usize newline = text.find('\n', start);
        printLine(text.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                       : newline - start));
        start = newline == std::string_view::npos ? text.size() : newline + 1;
    } while (start < text.size());
    if (m_output.size() > kScrollback) {
        m_output.erase(m_output.begin(), m_output.begin() + static_cast<std::ptrdiff_t>(
                                                                m_output.size() - kScrollback));
    }
    m_scroll = 0; // new output brings the view back to the bottom
}

void DevConsole::printLine(std::string_view line) {
    do {
        const usize length = breakAt(line, static_cast<usize>(kColumns));
        m_output.emplace_back(line.substr(0, length));
        line.remove_prefix(length);
        if (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1); // the break's own space
        }
    } while (!line.empty());
}

bool DevConsole::admits(const ConsoleCommand& command) const {
    return !ConsolePolicy::refusal(m_session, command.reach(), command.name()).has_value();
}

void DevConsole::run(std::string_view line) {
    print(std::format("{} {}", m_prompt, line));
    std::vector<std::string> parts = splitConsoleWords(line);
    if (parts.empty()) {
        return;
    }
    for (std::string& part : parts) {
        if (part.size() < 2 || part.front() != '$') {
            continue;
        }
        if (std::optional<std::string> value = variable(std::string_view{part}.substr(1))) {
            part = std::move(*value);
        } else {
            print(std::format("{} is not set", part));
            return;
        }
    }
    ConsoleCommand* command = m_commands.find(parts.front());
    if (command == nullptr) {
        print(kBadCommand);
        return;
    }
    const Arguments arguments = Arguments{parts}.subspan(1);
    if (ConsoleForm* form = m_commands.claimant(command->name(), arguments)) {
        if (const auto refused = ConsolePolicy::refusal(m_session, form->reach(), command->name(),
                                                        form->refusal())) {
            print(*refused);
        } else {
            form->run(*this, arguments);
        }
        return;
    }
    if (const auto refused = ConsolePolicy::refusal(m_session, command->reach(), command->name())) {
        print(*refused);
    } else {
        command->run(*this, arguments);
    }
}

void DevConsole::setVariable(std::string_view name, std::optional<std::string> value) {
    std::string key(name);
    for (char& c : key) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::erase_if(m_variables, [&](const auto& entry) { return entry.first == key; });
    if (value.has_value()) {
        m_variables.emplace_back(std::move(key), std::move(*value));
    }
}

std::optional<std::string> DevConsole::variable(std::string_view name) const {
    for (const auto& [key, value] : m_variables) {
        if (sameNameIgnoringCase(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

void DevConsole::help() {
    usize widest = 0;
    for (const auto& command : m_commands.commands()) {
        widest = std::max(widest, command->name().size());
    }
    for (const auto& command : m_commands.commands()) {
        if (!admits(*command)) {
            continue;
        }
        print(std::format("{:<{}}  {}", command->name(), widest, command->summary()));
    }
}

std::string DevConsole::suggestion() const {
    if (m_line.empty() || m_cursor != m_line.size() || m_line.back() == ' ') {
        return {};
    }
    const std::vector<std::string> parts = splitConsoleWords(m_line);
    if (parts.empty()) {
        return {};
    }
    if (parts.size() == 1) {
        // The command's name, among those the session admits.
        const auto name =
            m_commands.completeName(parts.front(), [this](std::string_view candidate) {
                const ConsoleCommand* named = m_commands.find(candidate);
                return named != nullptr && admits(*named);
            });
        return name ? name->substr(parts.front().size()) : std::string{};
    }
    // One of the command's own words, for the word being typed.
    const ConsoleCommand* command = m_commands.find(parts.front());
    if (command == nullptr || !admits(*command)) {
        return {};
    }
    const std::string& partial = parts.back();
    for (const std::string& word : command->completions(Arguments{parts}.subspan(1))) {
        if (word.size() > partial.size() && startsWithIgnoringCase(word, partial)) {
            return word.substr(partial.size());
        }
    }
    return {};
}

bool DevConsole::acceptSuggestion() {
    const std::string rest = suggestion();
    if (rest.empty()) {
        return false;
    }
    const usize room = static_cast<usize>(kColumns) - m_prompt.size() - 1;
    const bool commandWord = splitConsoleWords(m_line).size() == 1;
    std::string completed = m_line + rest + (commandWord ? " " : "");
    if (completed.size() > room) {
        completed.resize(room);
    }
    m_line = std::move(completed);
    m_cursor = m_line.size();
    return true;
}

void DevConsole::toggle() {
    m_open = !m_open;
    m_seeThrough = false;
    m_pointer.reset();
    if (m_open) {
        m_caretTime = 0.0;
    }
    for (HeldKey* key : {&m_backspace, &m_left, &m_right}) {
        key->held = 0.0;
    }
}

void DevConsole::close() {
    if (m_open) {
        toggle();
    }
    m_slide = 0;
    m_line.clear();
    m_pending.clear();
    m_cursor = 0;
    m_recalled = m_history.size();
}

s32 DevConsole::strokes(HeldKey& key, const Input& input, f64 deltaSeconds) {
    if (input.wasKeyPressed(key.key)) {
        key.held = 0.0;
        key.due = kRepeatDelay;
        return 1;
    }
    if (!input.isKeyDown(key.key)) {
        key.held = 0.0;
        return 0;
    }
    key.held += deltaSeconds;
    s32 count = 0;
    while (key.held >= key.due) {
        ++count;
        key.due += kRepeatInterval;
    }
    return count;
}

bool DevConsole::caretShown() const {
    return std::fmod(m_caretTime, 2.0 * kCaretHalfPeriod) < kCaretHalfPeriod;
}

void DevConsole::update(f64 deltaSeconds, const Input& input, std::span<const Key> toggleKeys) {
    const bool toggled =
        std::ranges::any_of(toggleKeys, [&](Key key) { return input.wasKeyPressed(key); });
    if (toggled) {
        toggle();
    }
    if (m_open) {
        readKeys(deltaSeconds, input, toggled);
    }
    m_slide =
        std::clamp(m_slide + (m_open ? deltaSeconds : -deltaSeconds) / kSlideSeconds, 0.0, 1.0);
    m_caretTime += deltaSeconds;
}

void DevConsole::readKeys(f64 deltaSeconds, const Input& input, bool toggled) {
    if (input.wasKeyPressed(Key::Escape)) {
        toggle();
        return;
    }
    const usize room = static_cast<usize>(kColumns) - m_prompt.size() - 1; // a cell for the caret
    if (!toggled) {
        for (const u32 code : input.typedText()) {
            if (code >= kFirstTypable && code <= kLastTypable && m_line.size() < room) {
                m_line.insert(m_cursor, 1, static_cast<char>(code));
                ++m_cursor;
            }
        }
    }
    for (s32 i = strokes(m_backspace, input, deltaSeconds); i > 0; --i) {
        eraseBeforeCursor();
    }
    for (s32 i = strokes(m_left, input, deltaSeconds); i > 0 && m_cursor > 0; --i) {
        --m_cursor;
    }
    // Right walks the caret along the line; at its end it takes the suggestion instead.
    for (s32 i = strokes(m_right, input, deltaSeconds); i > 0; --i) {
        if (m_cursor < m_line.size()) {
            ++m_cursor;
        } else if (!acceptSuggestion()) {
            break;
        }
    }
    if (input.wasKeyPressed(Key::Up)) {
        recall(-1);
    }
    if (input.wasKeyPressed(Key::Down)) {
        recall(1);
    }
    if (input.wasKeyPressed(Key::PageUp)) {
        scrollBy(kRows - 1);
    }
    if (input.wasKeyPressed(Key::PageDown)) {
        scrollBy(-(kRows - 1));
    }
    if (input.pointerScroll() != 0.0f) {
        scrollBy(static_cast<s32>(std::round(input.pointerScroll())) * kWheelLines);
    }
    if (input.wasKeyPressed(Key::Enter)) {
        const std::string line = std::move(m_line);
        m_line.clear();
        m_cursor = 0;
        if (!line.empty() && (m_history.empty() || m_history.back() != line)) {
            m_history.push_back(line);
            if (m_history.size() > kHistory) {
                m_history.erase(m_history.begin());
            }
        }
        m_recalled = m_history.size();
        run(line);
    }
}

void DevConsole::eraseBeforeCursor() {
    if (m_cursor > 0) {
        --m_cursor;
        m_line.erase(m_cursor, 1);
    }
}

void DevConsole::recall(s32 step) {
    const auto count = static_cast<s32>(m_history.size());
    const auto at = std::clamp(static_cast<s32>(m_recalled) + step, 0, count);
    if (at == static_cast<s32>(m_recalled)) {
        return;
    }
    if (m_recalled == m_history.size()) {
        m_pending = m_line; // leaving the new line for an older one
    }
    m_recalled = static_cast<usize>(at);
    m_line = m_recalled == m_history.size() ? m_pending : m_history[m_recalled];
    m_cursor = m_line.size();
}

usize DevConsole::shownLines() const {
    return std::min(m_output.size(), static_cast<usize>(kRows - 1));
}

void DevConsole::scrollBy(s32 lines) {
    const auto most = static_cast<s32>(m_output.size() - shownLines());
    m_scroll = static_cast<usize>(std::clamp(static_cast<s32>(m_scroll) + lines, 0, most));
}

void DevConsole::hover(Vec2 pointer, bool over) {
    if (!m_open) {
        m_seeThrough = false;
        m_pointer.reset();
        return;
    }
    if (!over) {
        m_seeThrough = false;
    } else if (m_pointer.has_value() && *m_pointer != pointer) {
        m_seeThrough = true;
    }
    m_pointer = pointer;
}

void DevConsole::draw(Canvas& canvas) const {
    const auto top = static_cast<f32>(std::round(-static_cast<f64>(kHeight) * (1.0 - m_slide)));
    // Seen through, the screen and its text go translucent so the view shows under them.
    Color paper = kPaper;
    Color ink = kInk;
    Color ghostInk = kGhostInk;
    if (m_seeThrough) {
        paper.a = kSeeThroughPaper;
        ink.a = kSeeThroughInk;
        ghostInk.a = kSeeThroughInk;
    }
    canvas.fill(Rect{0.0f, top, static_cast<f32>(kWidth), static_cast<f32>(kHeight)}, paper);
    if (!ready()) {
        return;
    }
    const auto rowY = [&](usize row) {
        return static_cast<s32>(top) + kMargin + static_cast<s32>(row) * kRowHeight;
    };
    const TextStyle style{.scale = kTextScale, .color = ink};
    const usize shown = shownLines();
    const usize first = m_output.size() - m_scroll - shown;
    for (usize row = 0; row < shown; ++row) {
        m_text.draw(canvas, kMargin, rowY(row), m_output[first + row], style);
    }
    const s32 y = rowY(shown);
    const s32 x = m_text.draw(canvas, kMargin, y, m_prompt, style) + kPromptGap;
    const s32 end = m_text.draw(canvas, x, y, m_line, style);
    // The suggestion stands after the line in dimmer ink, Right taking it.
    if (const std::string rest = suggestion(); !rest.empty()) {
        const TextStyle ghost{.scale = kTextScale, .color = ghostInk};
        m_text.draw(canvas, end, y, rest, ghost);
    }
    if (caretShown()) {
        const s32 caretX =
            x + m_text.measure(std::string_view(m_line).substr(0, m_cursor), kTextScale);
        canvas.fill(Rect{static_cast<f32>(caretX), static_cast<f32>(y + kRowHeight - kCaretHeight),
                         static_cast<f32>(kCellWidth), static_cast<f32>(kCaretHeight)},
                    ink);
    }
}

} // namespace gdl::game
