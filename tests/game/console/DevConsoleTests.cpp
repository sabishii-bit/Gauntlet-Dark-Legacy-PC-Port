#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleForm.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/DevConsole.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr std::array<Key, 1> kToggle{Key::Grave};
constexpr f64 kTick = 1.0 / 60.0;

TEST_CASE("console policy hides host commands and prevents their handlers running",
          "[console][online-console]") {
    DevConsole console;
    s32 calls = 0;
    console.addCommand("console_permissions", "host-only switch", ConsoleReach::HostOnly,
                       [&](DevConsole&, DevConsole::Arguments) { ++calls; });
    console.setSession({.seat = ConsoleSeat::Guest});
    console.run("help");
    REQUIRE(std::ranges::none_of(
        console.output(), [](const auto& line) { return line.contains("host-only switch"); }));
    console.run("CONSOLE_PERMISSIONS 1");
    REQUIRE(calls == 0);
    console.setSession({.seat = ConsoleSeat::Host});
    console.run("console_permissions 1");
    REQUIRE(calls == 1);
}

TEST_CASE("console dispatch routes expanded commands without mutating the local scene",
          "[console][online-console]") {
    DevConsole console;
    s32 localCalls = 0;
    std::string received;
    console.addCommand("get", "give an item", ConsoleReach::Local,
                       [&](DevConsole&, DevConsole::Arguments) { ++localCalls; });
    console.setVariable("ITEM", "gold");
    console.setDispatch([&](DevConsole&, std::string_view name, DevConsole::Arguments args) {
        REQUIRE(name == "get");
        REQUIRE(args.size() == 2);
        received = args[0] + " " + args[1];
        return true;
    });
    console.run("GET $ITEM 100");
    REQUIRE(received == "gold 100");
    REQUIRE(localCalls == 0);
}

/** A form of the test's own: it claims a command's words that begin with its word. */
class StandInForm final : public ConsoleForm {
public:
    StandInForm(std::string word, ConsoleReach reach, std::string refusal = {})
        : m_word(std::move(word)), m_reach(reach), m_refusal(std::move(refusal)) {}
    bool claims(Arguments arguments) const override {
        return !arguments.empty() && arguments.front() == m_word;
    }
    ConsoleReach reach() const override { return m_reach; }
    void run(DevConsole& console, Arguments arguments) override {
        runs += 1;
        argumentCount = static_cast<s32>(arguments.size());
        console.print(std::format("{} answered", m_word));
    }
    std::string_view refusal() const override { return m_refusal; }
    s32 runs = 0;
    s32 argumentCount = -1;

private:
    std::string m_word;
    ConsoleReach m_reach;
    std::string m_refusal;
};

TEST_CASE("a form that claims a command's words runs before the command, undispatched",
          "[dev-console][online-console]") {
    DevConsole console;
    s32 commandCalls = 0;
    s32 dispatched = 0;
    console.addCommand("get", "gives", ConsoleReach::Local,
                       [&](DevConsole&, DevConsole::Arguments) { ++commandCalls; });
    console.setDispatch([&](DevConsole&, std::string_view, DevConsole::Arguments) {
        ++dispatched;
        return false;
    });
    auto first = std::make_unique<StandInForm>("info", ConsoleReach::Local);
    auto second = std::make_unique<StandInForm>("info", ConsoleReach::Local);
    StandInForm* info = first.get();
    StandInForm* later = second.get();
    console.addForm("GET", std::move(first));
    console.addForm("get", std::move(second));
    console.addForm("get", nullptr); // nothing is registered
    const ConsoleRegistry& commands = console.commands();
    CHECK(commands.forms("Get").size() == 2);
    CHECK(commands.forms("goto").empty());
    const std::vector<std::string> gold{"gold", "10"};
    const std::vector<std::string> named{"info", "enemy#3"};
    CHECK(commands.claimant("get", gold) == nullptr);
    CHECK(commands.claimant("get", named) == info);
    console.run("GET info enemy#3");
    CHECK(info->runs == 1);
    CHECK(info->argumentCount == 2);
    CHECK(later->runs == 0);
    CHECK(commandCalls == 0);
    CHECK(dispatched == 0);
    CHECK(console.output().back() == "info answered");
    console.run("get gold 10");
    CHECK(info->runs == 1);
    CHECK(commandCalls == 1);
    CHECK(dispatched == 1);
    // A form on a name nobody registered answers nothing.
    console.addForm("fly", std::make_unique<StandInForm>("high", ConsoleReach::Local));
    console.run("fly high");
    CHECK(console.output().back() == DevConsole::kBadCommand);
}

TEST_CASE("an offline-only form says its own refusal outside offline play",
          "[dev-console][online-console]") {
    DevConsole console;
    s32 commandCalls = 0;
    console.addCommand("goto", "travels", ConsoleReach::Local,
                       [&](DevConsole&, DevConsole::Arguments) { ++commandCalls; });
    auto own = std::make_unique<StandInForm>("lab", ConsoleReach::OfflineOnly,
                                             "Stand-in maps are offline only.");
    auto plain = std::make_unique<StandInForm>("cave", ConsoleReach::OfflineOnly);
    StandInForm* lab = own.get();
    StandInForm* cave = plain.get();
    console.addForm("goto", std::move(own));
    console.addForm("goto", std::move(plain));
    const auto output = [&] {
        return std::vector<std::string>(console.output().begin(), console.output().end());
    };
    console.setSession({.seat = ConsoleSeat::Guest});
    console.run("goto lab");
    console.run("goto cave");
    console.run("goto G1");
    CHECK(output() == std::vector<std::string>{
                          "GDL> goto lab", "Stand-in maps are offline only.", "GDL> goto cave",
                          "This command is not available in this session.", "GDL> goto G1"});
    CHECK(lab->runs == 0);
    CHECK(cave->runs == 0);
    CHECK(commandCalls == 1);
    // Offline, under a preview that admits nothing of goto's own, the form still runs.
    constexpr std::array<std::string_view, 1> kHelpOnly{"help"};
    console.setSession({.modal = kHelpOnly});
    console.run("goto lab");
    CHECK(lab->runs == 1);
    console.run("goto G1");
    CHECK(console.output().back() == "This command is not available in this session.");
    CHECK(commandCalls == 1);
}

/** One poll of the keyboard: the keys held, the characters typed and the wheel's motion. */
struct Poll {
    std::vector<Key> down;
    std::string typed;
    f32 wheel = 0.0f;
};

void poll(Input& input, const Poll& keys) {
    input.beginPoll();
    for (const Key key : {Key::Grave, Key::Enter, Key::Backspace, Key::Escape, Key::A, Key::Up,
                          Key::Down, Key::Left, Key::Right, Key::PageUp, Key::PageDown}) {
        input.setKey(key, std::ranges::find(keys.down, key) != keys.down.end());
    }
    for (const char c : keys.typed) {
        input.addTypedChar(static_cast<u32>(static_cast<unsigned char>(c)));
    }
    if (keys.wheel != 0.0f) {
        input.scrollPointer(keys.wheel);
    }
}

void type(DevConsole& console, Input& input, std::string_view text) {
    for (const char c : text) {
        poll(input, {.typed = std::string(1, c)});
        console.update(kTick, input, kToggle);
    }
}

void press(DevConsole& console, Input& input, Key key) {
    poll(input, {.down = {key}});
    console.update(kTick, input, kToggle);
    poll(input, {});
    console.update(kTick, input, kToggle);
}

void enter(DevConsole& console, Input& input, std::string_view text) {
    type(console, input, text);
    press(console, input, Key::Enter);
}

TEST_CASE("revoking console access immediately releases input and unfinished text",
          "[console][online-console]") {
    DevConsole console;
    Input input;
    press(console, input, Key::Grave);
    enter(console, input, "help");
    type(console, input, "get gold 1000");
    REQUIRE(console.open());
    REQUIRE_FALSE(console.line().empty());
    const auto historySize = console.history().size();
    console.close();
    REQUIRE_FALSE(console.open());
    REQUIRE_FALSE(console.visible());
    REQUIRE(console.line().empty());
    REQUIRE(console.cursor() == 0);
    REQUIRE(console.history().size() == historySize);
    press(console, input, Key::Grave);
    REQUIRE(console.line().empty());
}

bool mentions(std::span<const std::string> lines, std::string_view text) {
    return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
}

struct Quad {
    f32 left = std::numeric_limits<f32>::max();
    f32 top = std::numeric_limits<f32>::max();
    f32 right = std::numeric_limits<f32>::lowest();
    f32 bottom = std::numeric_limits<f32>::lowest();
    usize vertices = 0;
};

/** The bounds of everything drawn untextured in `color`. */
Quad fillsOf(const test::FakeRenderDevice& device, Color color) {
    Quad quad;
    for (const auto& draw : device.draws) {
        if (draw.texture != &device.whiteTexture()) {
            continue;
        }
        for (const ImmediateVertex& v : draw.vertices) {
            if (v.color == color) {
                quad.left = std::min(quad.left, v.position.x);
                quad.top = std::min(quad.top, v.position.y);
                quad.right = std::max(quad.right, v.position.x);
                quad.bottom = std::max(quad.bottom, v.position.y);
                ++quad.vertices;
            }
        }
    }
    return quad;
}

/** The glyph sheet drawn from and how many cells were drawn. */
std::pair<const Texture*, usize> glyphsOf(const test::FakeRenderDevice& device) {
    usize glyphs = 0;
    const Texture* sheet = nullptr;
    for (const auto& draw : device.draws) {
        if (draw.texture == &device.whiteTexture()) {
            continue;
        }
        sheet = draw.texture;
        glyphs += draw.vertices.size() / 6;
    }
    return {sheet, glyphs};
}

void drawOnce(test::FakeRenderDevice& device, const DevConsole& console) {
    device.draws.clear();
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    console.draw(canvas);
    canvas.end();
}

TEST_CASE("the developer console is bound to the grave key and may be rebound",
          "[game][config][dev-console]") {
    CHECK(keyFromName("Grave") == Key::Grave);
    GameConfig config;
    CHECK(config.menu.console == std::vector{Key::Grave});
    config.mergeJson(R"({"controls":{"keyboard":{"console":["F3","Grave"]}}})");
    CHECK(config.menu.console == std::vector{Key::F3, Key::Grave});
    GameConfig saved;
    saved.mergeJson(config.toJson());
    CHECK(saved.menu.console == std::vector{Key::F3, Key::Grave});
    // The binding's former name still reads, for configurations saved before the console.
    GameConfig old;
    old.mergeJson(R"({"controls":{"keyboard":{"debug":["F4"]}}})");
    CHECK(old.menu.console == std::vector{Key::F4});
}

TEST_CASE("the console key slides the screen down over a quarter second and back up again",
          "[dev-console]") {
    DevConsole console;
    Input input;
    CHECK_FALSE(console.open());
    CHECK_FALSE(console.visible());
    poll(input, {.down = {Key::Grave}, .typed = "`"});
    console.update(0.1, input, kToggle);
    CHECK(console.open());
    CHECK(console.visible());
    CHECK(console.slide() == Approx(0.4f));
    CHECK(console.line().empty()); // the key's own character is not typed
    poll(input, {.down = {Key::Grave}});
    console.update(0.2, input, kToggle); // still held: no second toggle
    CHECK(console.open());
    CHECK(console.slide() == Approx(1.0f));
    poll(input, {});
    console.update(1.0, input, kToggle);
    CHECK(console.slide() == Approx(1.0f));
    poll(input, {.down = {Key::Grave}, .typed = "`"});
    console.update(0.125, input, kToggle);
    CHECK_FALSE(console.open());
    CHECK(console.visible());
    CHECK(console.slide() == Approx(0.5f));
    poll(input, {});
    console.update(0.125, input, kToggle);
    CHECK_FALSE(console.visible());
    CHECK(console.slide() == Approx(0.0f));
}

TEST_CASE("escape slides the console up and a toggle midway reverses the slide", "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    console.update(0.1, input, kToggle);
    REQUIRE(console.slide() == Approx(0.4f));
    console.toggle();
    console.update(0.05, input, kToggle);
    CHECK(console.slide() == Approx(0.2f));
    CHECK_FALSE(console.open());
    console.toggle();
    console.update(1.0, input, kToggle);
    REQUIRE(console.open());
    type(console, input, "abc");
    press(console, input, Key::Escape);
    CHECK_FALSE(console.open());
    CHECK(console.line() == "abc"); // the line waits for the console to come back
}

TEST_CASE("typed characters build the line, backspace trims it and enter runs it",
          "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    type(console, input, "debug_infp");
    CHECK(console.line() == "debug_infp");
    press(console, input, Key::Backspace);
    CHECK(console.line() == "debug_inf");
    type(console, input, "o");
    // Control characters and anything beyond ASCII are not in the font and are dropped.
    poll(input, {.typed = "\t\x01"});
    input.addTypedChar(0x00E9);
    console.update(kTick, input, kToggle);
    CHECK(console.line() == "debug_info");
    bool shown = false;
    std::vector<std::string> given;
    console.addCommand("debug_info", "shows or hides the level debug overlay", ConsoleReach::Local,
                       [&](DevConsole& self, DevConsole::Arguments arguments) {
                           shown = !shown;
                           given.assign(arguments.begin(), arguments.end());
                           self.print(shown ? "on" : "off");
                       });
    press(console, input, Key::Enter);
    CHECK(shown);
    CHECK(given.empty());
    CHECK(console.line().empty());
    REQUIRE(console.output().size() == 2);
    CHECK(console.output()[0] == "GDL> debug_info");
    CHECK(console.output()[1] == "on");
    // Names match ignoring case, as DOS matched them, and the rest of the line is passed on.
    console.run("  DEBUG_INFO  now   please ");
    CHECK_FALSE(shown);
    CHECK(given == std::vector<std::string>{"now", "please"});
    CHECK(console.output().back() == "off");
}

TEST_CASE("the prompt names whoever is at the keyboard", "[dev-console]") {
    DevConsole console;
    CHECK(console.prompt() == DevConsole::kDefaultPrompt);
    console.setPrompt("MERLIN>");
    console.run("help");
    CHECK(console.output().front() == "MERLIN> help");
}

TEST_CASE("help lists the commands and an unknown name is refused the way DOS refused one",
          "[dev-console]") {
    DevConsole console;
    console.addCommand("debug_info", "shows or hides the level debug overlay", ConsoleReach::Local,
                       [](DevConsole& /*self*/, DevConsole::Arguments /*arguments*/) {});
    console.run("help");
    CHECK(mentions(console.output(), "help        lists the commands"));
    CHECK(mentions(console.output(), "debug_info  shows or hides the level debug overlay"));
    const usize before = console.output().size();
    console.run("format c:");
    REQUIRE(console.output().size() == before + 2);
    CHECK(console.output()[before] == "GDL> format c:");
    CHECK(console.output()[before + 1] == DevConsole::kBadCommand);
    console.run("   ");
    CHECK(console.output().back() == "GDL>    "); // an empty line only echoes the prompt
}

TEST_CASE("a refused command says it is not available in this session",
          "[dev-console][online-console]") {
    DevConsole console;
    Input input;
    s32 calls = 0;
    console.addCommand(
        "party_count", "<1-4> or off: the party size the level sets itself up for",
        ConsoleReach::OfflineOnly,
        [&](DevConsole& /*self*/, DevConsole::Arguments /*arguments*/) { ++calls; },
        wordsCompleter({"1", "2", "3", "4", "off"}));
    console.setSession({.seat = ConsoleSeat::Host});
    const auto output = [&] {
        return std::vector<std::string>(console.output().begin(), console.output().end());
    };
    console.run("PARTY_COUNT 2");
    CHECK(calls == 0);
    CHECK(output() == std::vector<std::string>{"GDL> PARTY_COUNT 2",
                                               "This command is not available in this session."});
    console.run("help");
    // help pads the names to the widest registered, the refused one's included.
    CHECK(output() == std::vector<std::string>{"GDL> PARTY_COUNT 2",
                                               "This command is not available in this session.",
                                               "GDL> help", "help         lists the commands"});
    press(console, input, Key::Grave);
    REQUIRE(console.open());
    type(console, input, "pa");
    CHECK(console.suggestion().empty());
    type(console, input, "rty_count o");
    CHECK(console.suggestion().empty());
}

TEST_CASE("up and down walk the lines entered before and come back to the one being typed",
          "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    console.addCommand("second", "", ConsoleReach::Local,
                       [](DevConsole& /*self*/, DevConsole::Arguments /*arguments*/) {});
    enter(console, input, "first");
    enter(console, input, "second");
    enter(console, input, "second"); // a repeat of the last is not kept twice
    enter(console, input, "");       // nor an empty line
    REQUIRE(console.history().size() == 2);
    type(console, input, "thi");
    press(console, input, Key::Up);
    CHECK(console.line() == "second");
    press(console, input, Key::Up);
    CHECK(console.line() == "first");
    press(console, input, Key::Up); // nothing older
    CHECK(console.line() == "first");
    press(console, input, Key::Down);
    CHECK(console.line() == "second");
    press(console, input, Key::Down);
    CHECK(console.line() == "thi"); // the new line, set aside while browsing
    press(console, input, Key::Down);
    CHECK(console.line() == "thi");
    press(console, input, Key::Up);
    press(console, input, Key::Enter); // runs "second" again
    CHECK(console.output().back() == "GDL> second");
    CHECK(console.history().size() == 2);
    press(console, input, Key::Up);
    CHECK(console.line() == "second"); // browsing starts over from the newest
}

TEST_CASE("page up and page down and the wheel scroll the output, and new output comes back down",
          "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    const auto shown = static_cast<usize>(DevConsole::kRows - 1);
    for (usize i = 0; i < shown * 3; ++i) {
        console.print(std::to_string(i));
    }
    CHECK(console.scroll() == 0);
    press(console, input, Key::PageUp);
    CHECK(console.scroll() == shown);
    press(console, input, Key::PageUp);
    press(console, input, Key::PageUp); // clamped at the oldest line
    CHECK(console.scroll() == shown * 2);
    press(console, input, Key::PageDown);
    CHECK(console.scroll() == shown);
    poll(input, {.wheel = 1.0f});
    console.update(kTick, input, kToggle);
    CHECK(console.scroll() == shown + DevConsole::kWheelLines);
    poll(input, {.wheel = -2.0f});
    console.update(kTick, input, kToggle);
    CHECK(console.scroll() == shown - DevConsole::kWheelLines);
    console.print("new");
    CHECK(console.scroll() == 0);
    press(console, input, Key::PageDown); // nothing below the newest
    CHECK(console.scroll() == 0);
}

TEST_CASE("held backspace repeats after the typematic delay and the line keeps a caret cell",
          "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    const std::string fill(static_cast<usize>(DevConsole::kColumns), 'x');
    type(console, input, fill);
    const usize room =
        static_cast<usize>(DevConsole::kColumns) - DevConsole::kDefaultPrompt.size() - 1;
    CHECK(console.line().size() == room);
    poll(input, {.down = {Key::Backspace}});
    console.update(kTick, input, kToggle);
    CHECK(console.line().size() == room - 1);
    for (s32 tick = 0; tick < 24; ++tick) { // 0.4 s held: short of the delay
        poll(input, {.down = {Key::Backspace}});
        console.update(kTick, input, kToggle);
    }
    CHECK(console.line().size() == room - 1);
    poll(input, {.down = {Key::Backspace}});
    console.update(0.1 + DevConsole::kRepeatInterval * 2.5, input, kToggle);
    CHECK(console.line().size() == room - 1 - 3);
    poll(input, {});
    console.update(1.0, input, kToggle);
    CHECK(console.line().size() == room - 1 - 3);
}

TEST_CASE("left and right move the caret along the line, and typing goes in where it stands",
          "[dev-console]") {
    DevConsole console;
    Input input;
    console.toggle();
    type(console, input, "abc");
    CHECK(console.cursor() == 3);
    press(console, input, Key::Left);
    press(console, input, Key::Left);
    CHECK(console.cursor() == 1);
    type(console, input, "X");
    CHECK(console.line() == "aXbc");
    CHECK(console.cursor() == 2);
    press(console, input, Key::Right);
    CHECK(console.cursor() == 3);
    press(console, input, Key::Backspace);
    CHECK(console.line() == "aXc");
    CHECK(console.cursor() == 2);
    press(console, input, Key::Right);
    press(console, input, Key::Right); // no further than the end
    CHECK(console.cursor() == 3);
    // Held, Left repeats at the typematic rate after the delay.
    poll(input, {.down = {Key::Left}});
    console.update(kTick, input, kToggle);
    CHECK(console.cursor() == 2);
    for (s32 tick = 0; tick < 24; ++tick) { // 0.4 s held: short of the delay
        poll(input, {.down = {Key::Left}});
        console.update(kTick, input, kToggle);
    }
    CHECK(console.cursor() == 2);
    poll(input, {.down = {Key::Left}});
    console.update(0.1 + DevConsole::kRepeatInterval * 1.5, input, kToggle);
    CHECK(console.cursor() == 0);
    press(console, input, Key::Left); // no further than the start
    CHECK(console.cursor() == 0);
    // Backspace at the start erases nothing; a recalled line puts the caret at its end.
    press(console, input, Key::Backspace);
    CHECK(console.line() == "aXc");
    press(console, input, Key::Enter);
    CHECK(console.cursor() == 0);
    press(console, input, Key::Up);
    CHECK(console.line() == "aXc");
    CHECK(console.cursor() == 3);
}

TEST_CASE("the caret blinks at the VGA text cursor's rate from the moment the console opens",
          "[dev-console]") {
    DevConsole console;
    Input input;
    poll(input, {});
    console.update(DevConsole::kCaretHalfPeriod * 1.5, input, kToggle);
    console.toggle();
    CHECK(console.caretShown());
    console.update(DevConsole::kCaretHalfPeriod * 0.9, input, kToggle);
    CHECK(console.caretShown());
    console.update(DevConsole::kCaretHalfPeriod * 0.2, input, kToggle);
    CHECK_FALSE(console.caretShown());
    console.update(DevConsole::kCaretHalfPeriod, input, kToggle);
    CHECK(console.caretShown());
}

TEST_CASE("a line break in printed text ends the line", "[dev-console]") {
    DevConsole console;
    console.print("one\ntwo\n\nfour");
    REQUIRE(console.output().size() == 4);
    CHECK(console.output()[0] == "one");
    CHECK(console.output()[1] == "two");
    CHECK(console.output()[2].empty());
    CHECK(console.output()[3] == "four");
    console.print("five\n");
    CHECK(console.output().size() == 5);
    CHECK(console.output().back() == "five");
}

TEST_CASE("printed text wraps at spaces within the screen's columns and the scrollback is bounded",
          "[dev-console]") {
    DevConsole console;
    const std::string wide(static_cast<usize>(DevConsole::kColumns) + 5, 'w');
    console.print(wide); // no space to break at: hard
    REQUIRE(console.output().size() == 2);
    CHECK(console.output()[0].size() == static_cast<usize>(DevConsole::kColumns));
    CHECK(console.output()[1] == "wwwww");
    std::string prose;
    for (s32 i = 0; i < 40; ++i) {
        prose += "word ";
    }
    prose.pop_back(); // 199 characters of five-letter words
    console.print(prose);
    REQUIRE(console.output().size() == 4);
    CHECK(console.output()[2].size() <= static_cast<usize>(DevConsole::kColumns));
    CHECK(console.output()[2].ends_with("word"));
    CHECK(console.output()[3].starts_with("word"));
    console.print("");
    CHECK(console.output().back().empty());
    for (usize i = 0; i < DevConsole::kScrollback + 10; ++i) {
        console.print(std::to_string(i));
    }
    CHECK(console.output().size() == DevConsole::kScrollback);
    CHECK(console.output().back() == std::to_string(DevConsole::kScrollback + 9));
}

TEST_CASE("the screen is the status strip's width, black, and hangs above the view until lowered",
          "[dev-console]") {
    CHECK(DevConsole::kWidth == 512);
    CHECK(DevConsole::kWidth == StatusBoxPainter::kWidth * 4);
    DevConsole console;
    const Input input;
    test::FakeRenderDevice device;
    console.toggle();
    console.update(0.125, input, kToggle);
    REQUIRE(console.slide() == Approx(0.5f));
    drawOnce(device, console);
    const Quad screen = fillsOf(device, DevConsole::kPaper);
    CHECK(screen.vertices == 6);
    CHECK(screen.left == Approx(0.0f));
    CHECK(screen.right == Approx(512.0f));
    CHECK(screen.top == Approx(-96.0f));
    CHECK(screen.bottom == Approx(96.0f));
    // Without the font nothing but the screen is drawn, the caret included.
    CHECK_FALSE(console.ready());
    CHECK(fillsOf(device, DevConsole::kInk).vertices == 0);
    console.update(1.0, input, kToggle);
    drawOnce(device, console);
    const Quad down = fillsOf(device, DevConsole::kPaper);
    CHECK(down.top == Approx(0.0f));
    CHECK(down.bottom == Approx(192.0f));
}

TEST_CASE("the pointer moving over the screen sees through it until it leaves", "[dev-console]") {
    DevConsole console;
    const Input input;
    test::FakeRenderDevice device;
    console.toggle();
    console.update(1.0, input, kToggle);
    // Resting over the screen as it comes down changes nothing.
    console.hover(Vec2{0.5f, 0.1f}, true);
    CHECK_FALSE(console.seeThrough());
    console.hover(Vec2{0.5f, 0.1f}, true);
    CHECK_FALSE(console.seeThrough());
    // Moving over it does: the screen and its text go translucent.
    console.hover(Vec2{0.6f, 0.1f}, true);
    CHECK(console.seeThrough());
    drawOnce(device, console);
    CHECK(fillsOf(device, Color::rgba(0, 0, 0, DevConsole::kSeeThroughPaper)).vertices == 6);
    CHECK(fillsOf(device, DevConsole::kPaper).vertices == 0);
    // Leaving it brings the screen back; coming back over it sees through again.
    console.hover(Vec2{0.6f, 0.9f}, false);
    CHECK_FALSE(console.seeThrough());
    drawOnce(device, console);
    CHECK(fillsOf(device, DevConsole::kPaper).vertices == 6);
    console.hover(Vec2{0.6f, 0.1f}, true);
    CHECK(console.seeThrough());
    // Closing forgets it, and a pointer told of while the console is up means nothing.
    console.close();
    CHECK_FALSE(console.seeThrough());
    console.hover(Vec2{0.7f, 0.1f}, true);
    CHECK_FALSE(console.seeThrough());
}

TEST_CASE("the console writes its lines in half-size 8x8 cells behind an underscore caret",
          "[dev-console][assets]") {
    const auto root = test::unpackedOrSkip("STATIC/textures.json").parent_path().parent_path();
    test::unpackedOrSkip("fonts/font8x8.json");
    test::FakeRenderDevice device;
    DevConsole console;
    REQUIRE(console.load(device, root));
    CHECK(console.ready());
    Input input;
    console.toggle();
    console.update(1.0, input, kToggle);
    console.print("hello");
    type(console, input, "ab");
    while (!console.caretShown()) {
        console.update(kTick, input, kToggle);
    }
    drawOnce(device, console);
    const auto [sheet, glyphs] = glyphsOf(device);
    CHECK(sheet != nullptr);
    CHECK(glyphs == 5 + DevConsole::kDefaultPrompt.size() + 2);
    const Quad caret = fillsOf(device, DevConsole::kInk);
    CHECK(caret.vertices == 6);
    // After the prompt, its gap and two typed cells on the second row, one scanline deep.
    const auto cells = static_cast<f32>(DevConsole::kDefaultPrompt.size() + 2);
    CHECK(caret.left ==
          Approx(DevConsole::kMargin + cells * DevConsole::kCellWidth + DevConsole::kPromptGap));
    CHECK(caret.right == Approx(caret.left + DevConsole::kCellWidth));
    CHECK(caret.bottom == Approx(DevConsole::kMargin + 2 * DevConsole::kRowHeight));
    CHECK(caret.top == Approx(caret.bottom - DevConsole::kCaretHeight));
    // Moved back a cell, the caret is drawn there.
    press(console, input, Key::Left);
    while (!console.caretShown()) {
        console.update(kTick, input, kToggle);
    }
    drawOnce(device, console);
    const Quad moved = fillsOf(device, DevConsole::kInk);
    CHECK(moved.left == Approx(caret.left - DevConsole::kCellWidth));
    // Scrolled up a page with more output than rows, the prompt row stays and the newest
    // lines leave the screen.
    for (s32 i = 0; i < DevConsole::kRows * 2; ++i) {
        console.print(std::format("line {}", i));
    }
    press(console, input, Key::PageUp);
    drawOnce(device, console);
    CHECK(glyphsOf(device).second > glyphs);
    console.release();
    CHECK_FALSE(console.ready());
}

TEST_CASE("a line's $words stand for the console's variables, or the line is refused",
          "[dev-console]") {
    DevConsole console;
    Input input;
    std::vector<std::string> echoed;
    console.addCommand("echo", "says its arguments back", ConsoleReach::Local,
                       [&](DevConsole& self, DevConsole::Arguments arguments) {
                           for (const std::string& argument : arguments) {
                               echoed.push_back(argument);
                           }
                           self.print("echoed");
                       });
    press(console, input, Key::Grave);
    REQUIRE(console.open());
    CHECK_FALSE(console.variable("SELECTED").has_value());
    enter(console, input, "echo $SELECTED");
    CHECK(echoed.empty());
    CHECK(mentions(console.output(), "$SELECTED is not set"));
    CHECK_FALSE(mentions(console.output(), "echoed"));
    // Set, in any case, the word is its value wherever it stands in the line.
    console.setVariable("selected", "object#12");
    CHECK(console.variable("SELECTED") == "object#12");
    enter(console, input, "echo $selected plain $SELECTED");
    CHECK(echoed == std::vector<std::string>{"object#12", "plain", "object#12"});
    CHECK(mentions(console.output(), "echo $selected plain $SELECTED")); // echoed as typed
    // A lone dollar is a word of its own; set again, the new value stands; unset, it is gone.
    enter(console, input, "echo $");
    CHECK(echoed.back() == "$");
    console.setVariable("SELECTED", "enemy#3");
    CHECK(console.variable("selected") == "enemy#3");
    console.setVariable("SELECTED", std::nullopt);
    CHECK_FALSE(console.variable("SELECTED").has_value());
}

/** A command object of the test's own, counting what it was run with. */
class CountingCommand final : public ConsoleCommand {
public:
    explicit CountingCommand(std::string name) : m_name(std::move(name)) {}
    std::string_view name() const override { return m_name; }
    std::string_view summary() const override { return "counts its arguments"; }
    ConsoleReach reach() const override { return ConsoleReach::Local; }
    void run(DevConsole& /*console*/, Arguments arguments) override {
        runs += 1;
        argumentCount = static_cast<s32>(arguments.size());
    }
    std::vector<std::string> completions(Arguments /*arguments*/) const override {
        return {"one", "two"};
    }
    s32 runs = 0;
    s32 argumentCount = -1;

private:
    std::string m_name;
};

TEST_CASE("command objects register into the console and are found by name in any case",
          "[dev-console][autocomplete]") {
    DevConsole console;
    const ConsoleRegistry& commands = console.commands();
    const usize builtIn = commands.size(); // help stands registered from the start
    auto owned = std::make_unique<CountingCommand>("Spin");
    CountingCommand* spin = owned.get();
    console.addCommand(std::move(owned));
    console.run("SPIN a b");
    CHECK(spin->runs == 1);
    CHECK(spin->argumentCount == 2);
    CHECK(commands.find("spin") == spin);
    CHECK(commands.find("spinner") == nullptr);
    CHECK(commands.completeName("sp") == "Spin");
    CHECK_FALSE(commands.completeName("spin").has_value()); // nothing longer to offer
    CHECK_FALSE(commands.completeName("").has_value());
    CHECK_FALSE(commands.completeName("sp", [](std::string_view) { return false; }).has_value());
    // The command's own words complete too.
    CHECK(commands.find("spin")->completions({}) == std::vector<std::string>{"one", "two"});
    // Registering the name again replaces the command where it stood, in help's order.
    console.addCommand("later", "after it", ConsoleReach::Local,
                       [](DevConsole&, DevConsole::Arguments) {});
    console.addCommand("spin", "replaced", ConsoleReach::Local,
                       [](DevConsole&, DevConsole::Arguments) {});
    REQUIRE(commands.size() == builtIn + 2);
    CHECK(commands.commands()[builtIn]->name() == "spin");
    CHECK(commands.commands()[builtIn]->summary() == "replaced");
    CHECK(commands.commands()[builtIn + 1]->name() == "later");
    CHECK(commands.find("spin")->completions({}).empty());
}

TEST_CASE("the console suggests the rest of a word and Right at the end of the line takes it",
          "[dev-console][autocomplete]") {
    DevConsole console;
    Input input;
    console.addCommand("goto", "travels", ConsoleReach::OfflineOnly,
                       [](DevConsole&, DevConsole::Arguments) {});
    console.addCommand("get", "gives", ConsoleReach::Local,
                       [](DevConsole&, DevConsole::Arguments) {});
    console.addCommand(
        "list", "names", ConsoleReach::Local, [](DevConsole&, DevConsole::Arguments) {},
        wordsCompleter({"maps", "items"}));
    press(console, input, Key::Grave);
    REQUIRE(console.open());
    CHECK(console.suggestion().empty());
    type(console, input, "g");
    // The first registered name that fits, whatever the case typed.
    CHECK(console.suggestion() == "oto");
    type(console, input, "E");
    CHECK(console.suggestion() == "t");
    press(console, input, Key::Right);
    CHECK(console.line() == "gEt ");
    CHECK(console.cursor() == console.line().size());
    CHECK(console.suggestion().empty());
    // Away from the end of the line, Right only walks the caret.
    press(console, input, Key::Left);
    CHECK(console.suggestion().empty());
    press(console, input, Key::Right);
    CHECK(console.line() == "gEt ");
    CHECK(console.cursor() == 4);
    // A command's own words complete the argument being typed, without a space after.
    console.close();
    press(console, input, Key::Grave);
    type(console, input, "list it");
    CHECK(console.suggestion() == "ems");
    press(console, input, Key::Right);
    CHECK(console.line() == "list items");
    type(console, input, " x");
    CHECK(console.suggestion().empty());
    // A command the session refuses is not offered.
    console.close();
    press(console, input, Key::Grave);
    console.setSession({.seat = ConsoleSeat::Guest});
    type(console, input, "g");
    CHECK(console.suggestion() == "et");
    type(console, input, "oto ma");
    CHECK(console.suggestion().empty());
}

} // namespace
