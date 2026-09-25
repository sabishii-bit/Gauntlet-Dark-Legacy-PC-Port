#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/MessageTable.h"
#include "engine/assets/SoundSet.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/players/ClassData.h"
#include "game/players/PickupVoices.h"
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
    {"name": "SPEEDUP", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["SPEED BOOST"]},
    {"name": "MAGICUP", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["MAGIC BOOST"]},
    {"name": "POJOMSG", "font": 0, "scale": 1, "shadowScale": 1,
     "lines": ["POJO"]},
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

TEST_CASE("pickup announcements bypass the tutorial delay but retain priority and seen gates",
          "[game][help][items]") {
    MessageTable strings;
    loadStrings("help-pickup-delay", strings);
    HelpMessages help;
    help.setTexts(&strings);
    std::vector<s32> seen;
    const std::array party{HelpReader{0, &seen}};
    REQUIRE(help.post(1, 0, party) != nullptr);
    help.update(1000);
    REQUIRE(help.post(2, 0, party) != nullptr);
    help.update(1000); // The second message leaves a 120-tick tutorial delay.
    REQUIRE(help.post(32, 0, party) != nullptr);
    REQUIRE(help.post(33, 0, party) == nullptr); // Not over an equal-priority message.
    help.update(1000);
    REQUIRE(help.post(32, 0, party) == nullptr); // Seen; the pickup sound is separate.
    REQUIRE(help.post(33, 0, party) != nullptr);
    help.update(1000);
    REQUIRE(help.post(93, 0, party) != nullptr);
    help.update(1000);
    REQUIRE(help.post(93, 0, party) != nullptr); // Pojo is category zero: always.
}

TEST_CASE("item voices use retail solo and multiplayer name prefixes", "[game][help][items]") {
    using Lead = HelpMessages::VoiceLead;
    CHECK(HelpMessages::voiceLead(32, false) == Lead::None);
    CHECK(HelpMessages::voiceLead(32, true) == Lead::PlayerHas);
    CHECK(HelpMessages::voiceLead(89, false) == Lead::PlayerHas);
    CHECK(HelpMessages::voiceLead(93, false) == Lead::PlayerName);
    CHECK(HelpMessages::voiceLead(34, false) == Lead::PlayerHas);
    CHECK(HelpMessages::voiceLead(91, true) == Lead::None);
    CHECK(HelpMessages::voiceLead(113, true) == Lead::None);
    CHECK(HelpMessages::voiceLead(148, true) == Lead::None);
    CHECK(HelpMessages::specOf(148)->commonVoice);
    CHECK(HelpMessages::specOf(149)->voice == "S_PICKUPCRYST");
    CHECK(HelpMessages::specOf(150)->commonVoice);
    CHECK(HelpMessages::specOf(3)->voice == "S_MAGICFULL");
}

TEST_CASE("pickup help text and recordings resolve in the extracted retail banks",
          "[game][help][items][assets][unpacked]") {
    const auto root = test::unpackedOrSkip("text/english.json").parent_path().parent_path();
    const auto primaryDirectory = test::unpackedOrSkip("audio/VOICE1/sounds.json").parent_path();
    const auto secondaryDirectory = test::unpackedOrSkip("audio/VOICE2/sounds.json").parent_path();
    const auto commonDirectory = test::unpackedOrSkip("audio/COMMON/sounds.json").parent_path();
    const auto secretDirectory = test::unpackedOrSkip("audio/SECRET/sounds.json").parent_path();
    MessageTable strings;
    REQUIRE(strings.load(root / "text/english.json"));
    SoundSet primary;
    SoundSet secondary;
    SoundSet common;
    REQUIRE(primary.load(primaryDirectory));
    REQUIRE(secondary.load(secondaryDirectory));
    REQUIRE(common.load(commonDirectory));
    const std::array ids{3,  7,  15, 16, 28, 32, 33, 35, 36,  37,  38,  39,  40,  41, 42,
                         43, 47, 48, 49, 51, 52, 53, 54, 81,  82,  83,  84,  86,  87, 88,
                         89, 91, 92, 93, 94, 95, 98, 99, 100, 113, 132, 148, 149, 150};
    for (const s32 id : ids) {
        CAPTURE(id);
        const HelpMessageSpec* spec = HelpMessages::specOf(id);
        REQUIRE(spec != nullptr);
        CAPTURE(spec->text, spec->voice);
        REQUIRE(strings.find(spec->text).has_value());
        SoundSet* bank = &primary;
        if (spec->commonVoice) {
            bank = &common;
        } else if (!primary.find(spec->voice).has_value()) {
            bank = &secondary;
        }
        const auto sound = bank->find(spec->voice);
        REQUIRE(sound.has_value());
        CHECK_FALSE(bank->sequence(*sound).steps.empty());
    }
    for (const std::string_view name :
         {"S_LEVITATEUP", "S_GROW", "S_SHRINK", "S_POJO", "S_PICKUPSPECIAL", "S_PICKUPSHIELD",
          "S_PICKUPMAGIC", "S_PICKUPKEY", "S_PICKUPRUNE"}) {
        CAPTURE(name);
        const auto sound = common.find(name);
        REQUIRE(sound.has_value());
        CHECK_FALSE(common.sequence(*sound).steps.empty());
    }
    REQUIRE(primary.find("S_POJO2").has_value());
    REQUIRE(secondary.find("S_HAS").has_value());
    for (s32 character = 0; character < kStartingClassCount; ++character) {
        SoundSet voice;
        REQUIRE(voice.load(root / "audio" / classCode(character)));
        for (const std::string_view food : {"MEAT", "APPLE", "BANANA", "PINEAPPLE"}) {
            for (const bool spoken : {false, true}) {
                for (const bool poisoned : {false, true}) {
                    const PickupVoice cue =
                        PickupVoices::foodChoice(character, food, poisoned, false, spoken);
                    CAPTURE(cue.sound);
                    const auto sound = voice.find(cue.sound);
                    REQUIRE(sound.has_value());
                    CHECK_FALSE(voice.sequence(*sound).steps.empty());
                }
            }
        }
    }
    SoundSet secret;
    REQUIRE(secret.load(secretDirectory));
    for (s32 player = 0; player < 4; ++player) {
        for (const s32 amount : {50, 100, 500}) {
            const auto sound = secret.find(PickupVoices::bonusGold(player, amount));
            REQUIRE(sound.has_value());
            CHECK_FALSE(secret.sequence(*sound).steps.empty());
        }
    }
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
    REQUIRE(HelpMessages::specOf(HelpMessages::kFirstLegendName)->text == "TURBOBOOST");
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
