#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/screens/HelpMessages.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("gameplay tips exclude informational announcements", "[help][travel]") {
    CHECK(HelpMessages::gameplayTip(HelpMessages::kUseTurbo));
    CHECK(HelpMessages::gameplayTip(HelpMessages::kBarrelsHold));
    CHECK(HelpMessages::gameplayTip(HelpMessages::kTrapsHurt));
    CHECK_FALSE(HelpMessages::gameplayTip(HelpMessages::kLevelUp));
    CHECK_FALSE(HelpMessages::gameplayTip(HelpMessages::kFirstTurboName));
    CHECK_FALSE(HelpMessages::gameplayTip(HelpMessages::kHealthFull));
}

void loadStrings(std::string_view name, MessageTable& strings) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "english.json", R"({
  "fonts": ["font32"],
  "messages": [
    {"name": "USEKEYOPENDOOR", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["USE KEY", "TO OPEN DOORS"]},
    {"name": "USEKEYOPENCHEST", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["USE KEY TO OPEN", "TREASURE CHESTS"]},
    {"name": "HEALTHFULL", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["YOUR HEALTH IS FULL"]},
    {"name": "WAR_TURBO", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["TURBO ATTACK", "FIRE ARC", "PLASMA TRAIL"]},
    {"name": "LEVELUP", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["LEVEL %d", "EXPERIENCE"]}],
  "lists": []
})");
    REQUIRE(strings.load(dir / "english.json"));
}

TEST_CASE("the help messages are the original's, with the narrator's lines", "[game][help]") {
    const HelpMessageSpec* door = HelpMessages::specOf(HelpMessages::kDoorNeedsKey);
    REQUIRE(door != nullptr);
    REQUIRE(door->text == "USEKEYOPENDOOR");
    REQUIRE(door->voice == "S_USEKEY");
    REQUIRE(door->repeat == HelpRepeat::OnceForAll);
    REQUIRE(door->priority == 50);
    REQUIRE(HelpMessages::specOf(HelpMessages::kChestNeedsKey)->voice == "S_USEKEY2");
    REQUIRE(HelpMessages::specOf(HelpMessages::kHealthFull)->repeat == HelpRepeat::OncePerPlayer);
    // A class's turbo attacks are named from its own bank, the greater over the lesser.
    const HelpMessageSpec* fireArc = HelpMessages::specOf(57);
    REQUIRE(fireArc != nullptr);
    REQUIRE(fireArc->text == "WAR_TURBO");
    REQUIRE(fireArc->voice == "S_FIREARC");
    REQUIRE(fireArc->line == 1);
    REQUIRE(fireArc->classVoice);
    REQUIRE(fireArc->repeat == HelpRepeat::OncePerSession);
    REQUIRE(HelpMessages::specOf(58)->priority > fireArc->priority);
    REQUIRE(HelpMessages::specOf(79)->voice == "S_TURC_JES");
    REQUIRE(HelpMessages::specOf(56) == nullptr); // the strong attack has no name
    REQUIRE(HelpMessages::specOf(999) == nullptr);
    REQUIRE(HelpMessages::inkOf(1) == Color::rgba(0, 0, 0x1F));
    REQUIRE(HelpMessages::inkOf(7) == HelpMessages::inkOf(-1));
}

TEST_CASE("a help message goes up once for the party, a second a line and a half more",
          "[game][help]") {
    MessageTable strings;
    loadStrings("help-once", strings);
    HelpMessages help;
    std::vector<s32> first;
    std::vector<s32> second;
    const std::array<HelpReader, 2> party{HelpReader{0, &first}, HelpReader{2, &second}};
    // Without its strings it has nothing to say.
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 0, party) == nullptr);
    help.setTexts(&strings);
    REQUIRE(help.post(999, 0, party) == nullptr);
    const HelpMessageSpec* spec = help.post(HelpMessages::kDoorNeedsKey, 2, party);
    REQUIRE(spec != nullptr);
    REQUIRE(spec->voice == "S_USEKEY");
    REQUIRE(help.showing());
    REQUIRE(help.player() == 2);
    REQUIRE(help.lines() == std::vector<std::string>{"USE KEY", "TO OPEN DOORS"});
    REQUIRE(first == std::vector<s32>{HelpMessages::kDoorNeedsKey}); // everyone has seen it
    REQUIRE(second == first);
    // One at a time.
    REQUIRE(help.post(HelpMessages::kChestNeedsKey, 0, party) == nullptr);
    help.update(2 * HelpMessages::kTicksPerLine + HelpMessages::kTicksOver - 1);
    REQUIRE(help.showing());
    help.update(1);
    REQUIRE_FALSE(help.showing());
    // Seen, it does not come again; another may at once, the first pause being none.
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 0, party) == nullptr);
    REQUIRE(help.post(HelpMessages::kChestNeedsKey, 0, party) != nullptr);
    REQUIRE(first == std::vector<s32>{HelpMessages::kDoorNeedsKey, HelpMessages::kChestNeedsKey});
    help.update(1000);
    // After the second, a pause before the next.
    std::vector<s32> fresh;
    const std::array<HelpReader, 1> newcomer{HelpReader{1, &fresh}};
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 1, newcomer) == nullptr);
    help.update(HelpMessages::kPauses[1]);
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 1, newcomer) != nullptr);
    help.clear();
    REQUIRE_FALSE(help.showing());
}

TEST_CASE("the legend items are named by the realm they are for, and a rune held is news",
          "[game][help]") {
    const HelpMessageSpec* axe = HelpMessages::specOf(HelpMessages::kFirstLegendName + 2);
    REQUIRE(axe != nullptr);
    REQUIRE(axe->text == "LEGEND_ITEMS001");
    REQUIRE(axe->voice == "S_ICEAXEVOX");
    REQUIRE(HelpMessages::specOf(HelpMessages::kFirstLegendName + 1)->voice == "S_SCIMITARVOX");
    REQUIRE(HelpMessages::specOf(HelpMessages::kLastLegendName)->voice == "S_JAVELINVOX");
    REQUIRE(HelpMessages::specOf(HelpMessages::kFirstLegendName) == nullptr);
    const HelpMessageSpec* rune = HelpMessages::specOf(HelpMessages::kAlreadyHaveRune);
    REQUIRE(rune != nullptr);
    REQUIRE(rune->text == "ALREADYHAVERUNE");
    REQUIRE(rune->repeat == HelpRepeat::Always);
}

TEST_CASE("news is told every time, with its number filled in", "[game][help]") {
    MessageTable strings;
    loadStrings("help-news", strings);
    HelpMessages help;
    help.setTexts(&strings);
    std::vector<s32> seen;
    const std::array<HelpReader, 1> party{HelpReader{0, &seen}};
    const HelpMessageSpec* spec = help.post(HelpMessages::kLevelUp, 0, party, 12);
    REQUIRE(spec != nullptr);
    REQUIRE(spec->repeat == HelpRepeat::Always);
    REQUIRE(spec->voice == "S_GAINEDLEVEL");
    REQUIRE(help.lines().size() == 2);
    REQUIRE(help.lines()[0] == "LEVEL 12");
    REQUIRE(help.lines()[1] == "EXPERIENCE");
    // Told again for the next, though the same character has seen it.
    help.clear();
    help.setTexts(&strings);
    REQUIRE(help.post(HelpMessages::kLevelUp, 0, party, 13) != nullptr);
    REQUIRE(help.lines()[0] == "LEVEL 13");
    // Without a number the mark is left as it is.
    help.clear();
    help.setTexts(&strings);
    REQUIRE(help.post(HelpMessages::kLevelUp, 0, party) != nullptr);
    REQUIRE(help.lines()[0] == "LEVEL %d");
}

TEST_CASE("someone new to the party is told what the others already know", "[game][help]") {
    MessageTable strings;
    loadStrings("help-newcomer", strings);
    HelpMessages help;
    help.setTexts(&strings);
    std::vector<s32> old{HelpMessages::kDoorNeedsKey};
    std::vector<s32> fresh;
    const std::array<HelpReader, 2> party{HelpReader{0, &old}, HelpReader{1, &fresh}};
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 0, party) != nullptr);
    REQUIRE(fresh == std::vector<s32>{HelpMessages::kDoorNeedsKey});
    REQUIRE(old.size() == 1);
    help.clear();
    // A player's own message looks only at that player.
    std::vector<s32> mine{HelpMessages::kHealthFull};
    std::vector<s32> theirs;
    const std::array<HelpReader, 2> pair{HelpReader{0, &mine}, HelpReader{1, &theirs}};
    REQUIRE(help.post(HelpMessages::kHealthFull, 0, pair) == nullptr);
    REQUIRE(help.post(HelpMessages::kHealthFull, 1, pair) != nullptr);
    REQUIRE(theirs == std::vector<s32>{HelpMessages::kHealthFull});
}

TEST_CASE("a turbo attack is named once a session, over whatever lesson is up", "[game][help]") {
    MessageTable strings;
    loadStrings("help-turbo-names", strings);
    HelpMessages help;
    help.setTexts(&strings);
    std::vector<s32> seen;
    std::vector<s32> heard;
    const std::array<HelpReader, 1> party{HelpReader{0, &seen, &heard}};
    REQUIRE(help.post(HelpMessages::kDoorNeedsKey, 0, party) != nullptr);
    // The lesser attack's name takes the lesson's place, and shows its own line alone.
    const HelpMessageSpec* named = help.post(57, 0, party);
    REQUIRE(named != nullptr);
    REQUIRE(help.id() == 57);
    REQUIRE(help.lines() == std::vector<std::string>{"FIRE ARC"});
    REQUIRE(heard == std::vector<s32>{HelpMessages::kDoorNeedsKey, 57});
    // A lesson does not take a name's place, the greater attack's name does.
    REQUIRE(help.post(HelpMessages::kChestNeedsKey, 0, party) == nullptr);
    REQUIRE(help.post(58, 0, party) != nullptr);
    REQUIRE(help.lines() == std::vector<std::string>{"PLASMA TRAIL"});
    help.update(1000);
    // Heard this session, it is not said again, whatever pause the lessons are in.
    REQUIRE(help.post(57, 0, party) == nullptr);
    // Loaded afresh (nothing heard, all of it seen before), it is said once more.
    std::vector<s32> fresh;
    const std::array<HelpReader, 1> again{HelpReader{0, &seen, &fresh}};
    REQUIRE(help.post(57, 0, again) != nullptr);
    help.update(1000);
    // With someone in the party who has heard it, it is not said for anyone.
    std::vector<s32> none;
    std::vector<s32> newcomerHeard;
    const std::array<HelpReader, 2> pair{HelpReader{0, &seen, &fresh},
                                         HelpReader{1, &none, &newcomerHeard}};
    REQUIRE(help.post(57, 1, pair) == nullptr);
}

} // namespace
