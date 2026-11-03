#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleForm.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {

/**
 * The developer console: a black DOS-style screen that slides down from the top edge, as wide
 * as the party's four status boxes, and takes the keyboard while it is down. Lines typed at
 * its prompt in the game's 8x8 font, behind a blinking underscore caret, run the commands
 * registered with it; `help` lists them. Left and Right move the caret along the line,
 * Up and Down walk the lines typed before, Page Up and Page Down (or the wheel) the
 * output. The console key slides it down and back up. As a word is typed the console
 * suggests the rest of it in dimmer ink, a command's name or one of its own words; Right
 * at the end of the line takes the suggestion.
 */
class DevConsole {
public:
    using Arguments = ConsoleCommand::Arguments;
    using Handler = LambdaCommand::Handler;
    using Completer = LambdaCommand::Completer;

    static constexpr s32 kLanes = 4;
    static constexpr s32 kWidth = StatusBoxPainter::kWidth * kLanes; ///< the status strip's
    static constexpr s32 kHeight = 192;                              ///< half the 384-line screen
    static constexpr s32 kMargin = 4;
    /** FONT8X8 cells are eight wide and nine tall; the console writes them at half size. */
    static constexpr f32 kTextScale = 0.5f;
    static constexpr s32 kCellWidth = 4;
    static constexpr s32 kRowHeight = 5;
    static constexpr s32 kColumns = (kWidth - 2 * kMargin) / kCellWidth;
    static constexpr s32 kRows = (kHeight - 2 * kMargin) / kRowHeight; ///< the last is the prompt
    static constexpr usize kScrollback = 400;
    static constexpr usize kHistory = 100;
    static constexpr s32 kWheelLines = 3; ///< output lines a wheel notch scrolls
    static constexpr f64 kSlideSeconds = 0.25;
    /** The VGA text cursor toggles every sixteen retraces of its 70 Hz mode. */
    static constexpr f64 kCaretHalfPeriod = 16.0 / 70.0;
    /** The cursor is the bottom scanline of the cell. */
    static constexpr s32 kCaretHeight = 1;
    /** The BIOS typematic defaults: half a second, then 10.9 repeats a second. */
    static constexpr f64 kRepeatDelay = 0.5;
    static constexpr f64 kRepeatInterval = 1.0 / 10.9;
    static constexpr std::string_view kDefaultPrompt = "GDL>";
    /** Half a cell between the prompt and the line typed, so they do not read as one word. */
    static constexpr s32 kPromptGap = kCellWidth / 2;
    static constexpr std::string_view kBadCommand = "Bad command or file name";
    static constexpr Color kInk = Color::rgba(170, 170, 170);   ///< the text mode's light grey
    static constexpr Color kGhostInk = Color::rgba(85, 85, 85); ///< a suggestion's, dimmer
    static constexpr Color kPaper = Color::rgba(0, 0, 0);
    static constexpr u8 kSeeThroughPaper = 112; ///< the screen's alpha while seen through
    static constexpr u8 kSeeThroughInk = 208;   ///< and its text's

    DevConsole();
    ~DevConsole() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(DevConsole);

    /** Loads the 8x8 font from the unpacked assets; without it the screen is drawn blank. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void release();
    bool ready() const { return m_text.ready(); }

    /** Registers a command object; one of the same name replaces the one held. */
    void addCommand(std::unique_ptr<ConsoleCommand> command);
    /** Registers `name`, matched ignoring case; `summary` is what `help` says of it, `reach`
     * where it runs, and `completer` names the words the console may complete its arguments
     * to. */
    void addCommand(std::string name, std::string summary, ConsoleReach reach, Handler handler,
                    Completer completer = {});
    /** Registers `form` on the command named `command`, tried before the command itself. */
    void addForm(std::string_view command, std::unique_ptr<ConsoleForm> form);
    const ConsoleRegistry& commands() const { return m_commands; }
    /** What the application is doing; the policy judges each command and form by it in help,
     * in the suggestions and when a line runs. */
    void setSession(ConsoleSession session) { m_session = session; }
    /** The session set last, which a running command may judge its words by. */
    const ConsoleSession& session() const { return m_session; }
    /** Appends output, a line per line break, each wrapped to the screen's columns at
     * spaces where it can be. */
    void print(std::string_view text);
    /** Echoes `line` under the prompt and runs it: the first word names the command, the
     * rest are its arguments, which the first of the command's forms to claim them answers
     * instead of the command. An unknown name is refused the way DOS refused one, and one
     * the session does not admit is refused in the policy's words. */
    void run(std::string_view line);
    /** What stands before the line being typed, such as the character's name and `>`. */
    void setPrompt(std::string prompt) { m_prompt = std::move(prompt); }
    const std::string& prompt() const { return m_prompt; }
    /** Gives `$name` (matched ignoring case) a value for a line's words to stand for, or
     * takes it away. A line naming one that has none is refused, having said so. */
    void setVariable(std::string_view name, std::optional<std::string> value);
    std::optional<std::string> variable(std::string_view name) const;

    /** Slides the console the other way; coming down, the caret starts shown. */
    void toggle();
    /** Revocation closes immediately and discards unfinished text, not history. */
    void close();
    /** Whether the console holds the keyboard: it is down or coming down. */
    bool open() const { return m_open; }
    /** Whether any of the screen shows. */
    bool visible() const { return m_slide > 0.0; }
    /** How far down the screen is, 0 above the view to 1 fully down. */
    f32 slide() const { return static_cast<f32>(m_slide); }
    bool caretShown() const;
    /** Where the pointer is each frame and whether it is over the screen. Once the pointer
     * moves while over the screen the console lets the view show through it, so a click
     * there may pick what is beneath, until the pointer leaves the screen or the console
     * closes. A pointer resting over the screen as it comes down changes nothing. */
    void hover(Vec2 pointer, bool over);
    bool seeThrough() const { return m_seeThrough; }
    const std::string& line() const { return m_line; }
    /** Where the caret stands in the line: how many characters are before it. */
    usize cursor() const { return m_cursor; }
    std::span<const std::string> output() const { return m_output; }
    std::span<const std::string> history() const { return m_history; }
    /** How many lines above the newest the output is scrolled to. */
    usize scroll() const { return m_scroll; }
    /** The rest of the word being typed, as the console would complete it: a command's
     * name while the first word is typed, else one of the command's own words; nothing
     * when there is none or the caret is not at the end of the line. */
    std::string suggestion() const;
    /** Takes the suggestion into the line, with a space after a completed command name. */
    bool acceptSuggestion();

    /** Advances the slide and the caret and, while open, takes the frame's keys and text. A
     * press of one of `toggleKeys` slides the console the other way, the character it types
     * dropped; Escape slides it up. Backspace, Left and Right repeat while held; Right at
     * the end of the line takes the suggestion instead. */
    void update(f64 deltaSeconds, const Input& input, std::span<const Key> toggleKeys);
    /** Draws the screen, the output above the prompt line, the suggestion and the caret, in
     * virtual pixels. */
    void draw(Canvas& canvas) const;

private:
    /** A key that repeats while held, at the typematic rate. */
    struct HeldKey {
        Key key = Key::Unknown;
        f64 held = 0.0; ///< how long it has been down
        f64 due = 0.0;  ///< when, on that clock, it next repeats
    };
    void readKeys(f64 deltaSeconds, const Input& input, bool toggled);
    /** How many times `key` acts this poll: once pressed, then by the typematic rate. */
    static s32 strokes(HeldKey& key, const Input& input, f64 deltaSeconds);
    void eraseBeforeCursor();
    void printLine(std::string_view line);
    void recall(s32 step);
    void scrollBy(s32 lines);
    /** How many output lines show above the prompt. */
    usize shownLines() const;
    void help();
    /** Whether the session runs `command` and offers it in help and the suggestions. */
    bool admits(const ConsoleCommand& command) const;

    BitmapFont m_font;
    TextureSet m_textures;
    TextPainter m_text;
    ConsoleRegistry m_commands;
    ConsoleSession m_session;
    std::vector<std::string> m_output;
    std::vector<std::string> m_history;
    std::vector<std::pair<std::string, std::string>> m_variables; ///< by upper-case name
    usize m_recalled = 0;  ///< the history line shown; the count when typing a new one
    std::string m_pending; ///< the new line set aside while an older one is shown
    std::string m_line;
    usize m_cursor = 0; ///< the caret's place in the line
    std::string m_prompt{kDefaultPrompt};
    usize m_scroll = 0;
    bool m_open = false;
    f64 m_slide = 0.0;
    f64 m_caretTime = 0.0;
    std::optional<Vec2> m_pointer; ///< where the pointer was last, over the screen or not
    bool m_seeThrough = false;
    HeldKey m_backspace{Key::Backspace};
    HeldKey m_left{Key::Left};
    HeldKey m_right{Key::Right};
};

} // namespace gdl::game
