#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/app/LegendsConsoleWords.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "game/console/ConsoleActionSink.h"
#include "game/console/ConsoleListings.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/level/LevelCatalog.h"
#include "game/scenes/SharedConsoleCommand.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

constexpr std::string_view kLegendsOffline = "Legends maps are offline only.";

/** What goto's usage says while a Legends map is installed or visited: every form, the note. */
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]            travels to a level, standing at a point when given\n"
    "goto x y z                    moves the party to that point here\n"
    "goto <legends map> [preview]  walks an installed Legends map, or flies through it\n"
    "goto n64|dreamcast <map>      the same, naming the map's platform\n"
    "goto back                     returns from a Legends map to the level left for it\n"
    "list maps names the levels and the Legends maps installed.";
/** And with none: goto's own forms alone. */
constexpr std::string_view kBaseGotoUsage =
    "goto <map> [x y z]  travels to a level, standing at a point when given\n"
    "goto x y z          moves the party to that point here";
constexpr std::string_view kGotoSummary =
    "<map> [x y z], x y z or <legends map> [preview]: brings the party there";
constexpr std::string_view kBaseGotoSummary = "<map> [x y z] or x y z: brings the party there";

/** Where the Legends maps stand: none, five on the N64, or one visited with none installed. */
enum class Maps : u8 { Absent, Installed, Visiting };

/** A catalogue of three realms and the tower, kept as the old wdata files. */
std::filesystem::path syntheticRealms() {
    const auto dir = test::scratchDirectory("legends-console-realms");
    std::filesystem::create_directories(dir / "wdata");
    writeTextFile(dir / "wdata/TRAINING.json", R"({"realm": 0, "prefix": "levelT", "levels": [
      {"name": "T1", "title": "Yard"}, {"name": "T2", "title": "Hall"},
      {"name": "T3", "title": "Roof"}]})");
    writeTextFile(dir / "wdata/BATTLE.json", R"({"realm": 2, "prefix": "levelB", "levels": [
      {"name": "B1", "title": "One"}, {"name": "B2", "title": "Two"},
      {"name": "B3", "title": "Three"}, {"name": "B4", "title": "Four"},
      {"name": "B5", "title": "Five"}, {"name": "B6", "title": "Six"}]})");
    writeTextFile(dir / "wdata/KEEP.json", R"({"realm": 10, "prefix": "levelK", "levels": [
      {"name": "K2", "title": "Gate"}, {"name": "K3", "title": "Wall"},
      {"name": "K1", "title": "Keep"}]})");
    writeTextFile(dir / "wdata/TOWER.json", R"({"realm": 13, "prefix": "levelL", "levels": [
      {"name": "L1", "title": "Tower"}]})");
    return dir;
}

/** A sink of the test's own: it keeps the lines it is given. */
class RecordingSink final : public ConsoleActionSink {
public:
    std::optional<std::string> submit(std::string line) override {
        lines.push_back(std::move(line));
        return std::nullopt;
    }

    std::vector<std::string> lines;
};

/** A console with the shared verbs over a synthetic catalogue and the expansion's words over
 * made-up maps, counting what the words ask of them. */
struct Console {
    /** What the fake maps were asked and did. */
    struct Asked {
        s32 rescans = 0;  ///< installed asked to walk the disk again
        s32 looks = 0;    ///< installed asked as found
        Words opened;     ///< "<platform> <folder> walk|preview"
        s32 closed = 0;   ///< the way back taken
        s32 unlocked = 0; ///< the native tower opened
    };

    Maps maps;
    LevelCatalog levels;
    RecordingSink sink;
    ConsoleListings listings;
    DevConsole console;
    Asked asked;
    bool walkingNative = false;

    explicit Console(Maps where) : maps(where) {
        REQUIRE(levels.load(syntheticRealms()));
        console.addCommand(listCommand(listings));
        registerSharedConsoleCommands(console.host(), console.commands(), sink, levels);
        registerLegendsConsoleWords(
            console.host(), listings,
            {.visiting = [this] { return maps == Maps::Visiting; },
             .installed =
                 [this](bool rescan) {
                     (rescan ? asked.rescans : asked.looks) += 1;
                     return maps == Maps::Installed;
                 },
             .mapNames =
                 [this](LegendsPlatform platform) {
                     return maps == Maps::Installed && platform == LegendsPlatform::N64
                                ? Words{"battle1", "boss1", "castle1", "tower2", "town1"}
                                : Words{};
                 },
             .find = [](std::string_view /*name*/, std::optional<LegendsPlatform> /*platform*/)
                 -> std::optional<std::pair<LegendsPlatform, std::filesystem::path>> {
                 return std::nullopt;
             },
             .open =
                 [this](DevConsole& /*shown*/, LegendsPlatform platform,
                        const std::filesystem::path& directory, bool walk) {
                     asked.opened.push_back(std::format("{} {} {}", legendsPlatformName(platform),
                                                        directory.generic_string(),
                                                        walk ? "walk" : "preview"));
                 },
             .close = [this](DevConsole& /*shown*/) { asked.closed += 1; },
             .walkingNative = [this] { return walkingNative; },
             .unlockTower = [this](DevConsole& /*shown*/) { asked.unlocked += 1; }});
    }

    std::string say(std::string_view line, const ConsoleSession& session = {}) {
        console.setSession(session);
        return test::said(console, line);
    }
    std::string suggestion(std::string_view typed) { return test::suggestionAfter(console, typed); }
    const SharedConsoleCommand& go() const {
        const auto* found =
            dynamic_cast<const SharedConsoleCommand*>(console.commands().find("goto"));
        REQUIRE(found != nullptr);
        return *found;
    }
    Words words(const Words& arguments) const {
        return console.commands().completions(go(), arguments);
    }
};

TEST_CASE("a word names a Legends platform, and a platform its maps on a line",
          "[legends][online-console]") {
    CHECK(legendsPlatformOf("Dreamcast") == LegendsPlatform::Dreamcast);
    CHECK(legendsPlatformOf("DC") == LegendsPlatform::Dreamcast);
    CHECK(legendsPlatformOf("N64") == LegendsPlatform::N64);
    CHECK_FALSE(legendsPlatformOf("gamecube").has_value());
    CHECK(legendsPlatformName(LegendsPlatform::N64) == "N64");
    CHECK(legendsPlatformName(LegendsPlatform::Dreamcast) == "Dreamcast");
    CHECK(legendsMapLines(LegendsPlatform::N64, {}).empty());
    const Words names{"castle", "forest"};
    CHECK(legendsMapLines(LegendsPlatform::Dreamcast, names) ==
          Words{"Dreamcast Legends maps (goto <name>): castle, forest"});
    CHECK(legendsMapLines(LegendsPlatform::N64, names) ==
          Words{"N64 Legends maps (goto <name>): castle, forest"});
    // A preview admits help and goto alone.
    CHECK(kLegendsPreviewWords == std::array<std::string_view, 2>{"help", "goto"});
}

TEST_CASE("installed Legends maps keep goto's help, usage and words as they are today",
          "[legends][online-console]") {
    Console fixture{Maps::Installed};
    CHECK(fixture.go().summary() == kGotoSummary);
    CHECK(fixture.go().usage() == kGotoUsage);
    CHECK(fixture.say("goto") == test::shownAs(kGotoUsage));
    CHECK(fixture.say("help").contains(
        test::shownAs(std::format("{:<12}  {}", "goto", kGotoSummary))));
    CHECK(fixture.words({"t"}) == Words{"tower",   "back",  "T1",      "T2",     "T3",
                                        "B1",      "B2",    "B3",      "B4",     "B5",
                                        "B6",      "K2",    "K3",      "K1",     "L1",
                                        "battle1", "boss1", "castle1", "tower2", "town1"});
    CHECK(fixture.words({"x", "p"}) == Words{"preview"});
    CHECK(fixture.words({"x", "p", "q"}).empty());
    CHECK(fixture.suggestion("goto b") == "ack");
    CHECK(fixture.suggestion("goto t") == "ower");
    CHECK(fixture.suggestion("goto k") == "2");
    CHECK(fixture.suggestion("goto tower") == "2");
    CHECK(fixture.suggestion("goto ba") == "ck");
    CHECK(fixture.suggestion("goto x p") == "review");
    CHECK(fixture.suggestion("goto 1 p") == "review");
    // list maps names them afresh, a platform without any saying nothing.
    const s32 rescans = fixture.asked.rescans;
    CHECK(fixture.say("list maps") ==
          "N64 Legends maps (goto <name>): battle1, boss1, castle1, tower2, town1");
    CHECK(fixture.asked.rescans == rescans + 1);
}

TEST_CASE("with no Legends map installed, goto names none of their words",
          "[legends][online-console]") {
    Console fixture{Maps::Absent};
    CHECK(fixture.go().summary() == kBaseGotoSummary);
    CHECK(fixture.go().usage() == kBaseGotoUsage);
    const std::string help = fixture.say("help");
    CHECK(help.contains(test::shownAs(std::format("{:<12}  {}", "goto", kBaseGotoSummary))));
    CHECK_FALSE(help.contains("legends"));
    for (const ConsoleSession& session :
         {ConsoleSession{}, ConsoleSession{.role = ConsoleRole::Guest}}) {
        CHECK(fixture.say("goto", session) == test::shownAs(kBaseGotoUsage));
        CHECK(fixture.say("goto n64 1 2 3", session) == test::shownAs(kBaseGotoUsage));
    }
    CHECK(fixture.words({"t"}) == Words{"tower", "T1", "T2", "T3", "B1", "B2", "B3", "B4", "B5",
                                        "B6", "K2", "K3", "K1", "L1"});
    CHECK(fixture.words({"x", "p"}).empty());
    CHECK(fixture.suggestion("goto b") == "1");
    CHECK(fixture.suggestion("goto x p").empty());
    CHECK(fixture.say("list maps").empty());
    // The words are claimed and answered as before, whatever is installed.
    CHECK(fixture.say("goto castle") ==
          "No level or Legends map named castle; list maps names them");
    CHECK(fixture.say("goto C:/x/castle1") ==
          "Name the platform with a path: goto n64|dreamcast <path>");
    CHECK(fixture.say("goto back").empty());
    CHECK(fixture.asked.closed == 1);
    CHECK(fixture.say("goto n64 C:/x/castle1").empty());
    CHECK(fixture.say("goto dc C:/x/castle1 preview").empty());
    CHECK(fixture.asked.opened == Words{"N64 C:/x/castle1 walk", "Dreamcast C:/x/castle1 preview"});
    for (const std::string_view line : {"goto castle", "goto back", "goto n64 C:/x/castle1"}) {
        CAPTURE(line);
        CHECK(fixture.say(line, {.role = ConsoleRole::Guest}) == kLegendsOffline);
        CHECK(fixture.say(line, {.role = ConsoleRole::Host}) == kLegendsOffline);
    }
    CHECK(fixture.asked.closed == 1);
    CHECK(fixture.asked.opened.size() == 2);
    CHECK(fixture.sink.lines.empty());
    // Neither the claims, the words nor help walked the disk again; list maps did, once.
    CHECK(fixture.asked.rescans == 1);
    CHECK(fixture.asked.looks > 0);
}

TEST_CASE("a Legends map visited offers the way back and its preview with none installed",
          "[legends][online-console]") {
    Console fixture{Maps::Visiting};
    CHECK(fixture.go().summary() == kGotoSummary);
    CHECK(fixture.go().usage() == kGotoUsage);
    CHECK(fixture.suggestion("goto b") == "ack");
    CHECK(fixture.suggestion("goto x p") == "review");
    CHECK(fixture.asked.rescans == 0);
    // Under its preview, goto's words run and the level's own wait.
    const ConsoleSession preview =
        ConsolePolicy::session({.preview = kLegendsPreviewWords, .parked = true});
    CHECK(fixture.say("goto back", preview).empty());
    CHECK(fixture.asked.closed == 1);
    CHECK(fixture.say("goto G1", preview) == ConsolePolicy::kRefused);
    CHECK(fixture.say("unlock_tower", preview) == ConsolePolicy::kRefused);
    CHECK(fixture.sink.lines.empty());
}

TEST_CASE("unlock_tower opens a native world's own tower only while one is walked offline",
          "[legends][online-console]") {
    Console fixture{Maps::Installed};
    CHECK(fixture.say("unlock_tower").empty());
    CHECK(fixture.sink.lines == Words{"unlock_tower"});
    CHECK(fixture.asked.unlocked == 0);
    fixture.walkingNative = true;
    CHECK(fixture.say("unlock_tower").empty());
    CHECK(fixture.say("unlock_tower again").empty());
    CHECK(fixture.asked.unlocked == 2);
    CHECK(fixture.sink.lines == Words{"unlock_tower"});
    // A preview's words hold it as they hold the shared verb.
    const ConsoleSession preview =
        ConsolePolicy::session({.preview = kLegendsPreviewWords, .parked = true});
    CHECK(fixture.say("unlock_tower", preview) == ConsolePolicy::kRefused);
    CHECK(fixture.asked.unlocked == 2);
    // help names unlock_tower once, as the verb says it.
    CHECK(fixture.say("help").contains(test::shownAs(
        std::format("{:<12}  {}", "unlock_tower", "opens every wing and portal of the tower"))));
}

} // namespace
