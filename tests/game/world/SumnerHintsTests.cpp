#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/world/SumnerHints.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** Hint texts shaped like the game's: a title message and a list per topic, every guardian,
 * item and runestone entry named after its place in its list, three passages each. */
std::filesystem::path sampleHints(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::string messages;
    const auto add = [&](const std::string& id, const std::vector<std::string>& lines, f32 scale) {
        if (!messages.empty()) {
            messages += ",\n";
        }
        messages += R"({"name": ")" + id + R"(", "font": 0, "scale": )" + std::to_string(scale) +
                    R"(, "lines": [)";
        for (usize i = 0; i < lines.size(); ++i) {
            messages += (i == 0 ? "\"" : ", \"") + lines[i] + "\"";
        }
        messages += "]}";
    };
    // Messages 0..12 guardians, 13..25 runestones, 26..38 items, 39..42 general.
    for (const std::string topic : {"G", "R", "L"}) {
        for (s32 i = 0; i < 13; ++i) {
            const std::string id = topic + std::to_string(i);
            add(id, {id + " one", id + " two", id + " three"}, 0.8f);
        }
    }
    add("GEN1", {"first", "second"}, 1.0f);
    add("GEN2", {"west"}, 1.0f);
    add("GEN3", {"east"}, 1.0f);
    add("GEN4", {"battle"}, 1.0f);
    std::vector<std::string> titles;
    titles.reserve(13);
    for (s32 i = 0; i < 13; ++i) {
        titles.push_back("title " + std::to_string(i));
    }
    add("BOSSHINTDESC", titles, 0.8f);
    add("RUNEHINTDESCS", titles, 0.8f);
    add("LEGENDHINTDESCS", titles, 0.8f);
    const auto run = [](s32 from, s32 count) {
        std::string out;
        for (s32 i = 0; i < count; ++i) {
            out += (i == 0 ? "" : ", ") + std::to_string(from + i);
        }
        return out;
    };
    writeTextFile(dir / "hints.json",
                  "{\"fonts\": [\"font32\"], \"messages\": [\n" + messages +
                      "],\n\"lists\": [\n{\"name\": \"BOSS_HINTS\", \"messages\": [" + run(0, 13) +
                      "]},\n{\"name\": \"RUNE_HINTS\", \"messages\": [" + run(13, 13) +
                      "]},\n{\"name\": \"LEGEND_HINTS\", \"messages\": [" + run(26, 13) +
                      "]},\n{\"name\": \"GENERAL_HINTS\", \"messages\": [" + run(39, 4) + "]}]}");
    return dir / "hints.json";
}

u32 worlds(std::initializer_list<s32> ids) {
    u32 bits = 0;
    for (const s32 id : ids) {
        bits |= 1U << static_cast<u32>(id);
    }
    return bits;
}

TEST_CASE("general hints run through the furthest open wing's list, then the ones before",
          "[game][world][hints]") {
    SumnerHints hints;
    REQUIRE_FALSE(hints.loaded());
    REQUIRE(hints.next(HintTopic::General, {}, "A HINT").passages.empty());
    REQUIRE(hints.load(sampleHints("hints-general")));
    HintKnowledge knowledge;
    // With no wing open only the first list is told, over and over.
    HintPage page = hints.next(HintTopic::General, knowledge, "A HINT");
    REQUIRE(page.title == "A HINT");
    REQUIRE(page.centred);
    REQUIRE(page.scale == Approx(1.0f));
    REQUIRE(page.passages == std::vector<std::string>{"first"});
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "second");
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "first");
    // A visit does not start them over.
    hints.beginVisit();
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "second");
    // With the first two wings open, the round starts from the east wing's list, then the
    // west's, then the first again; the battle grounds' stays untold.
    knowledge.wingsOpen = {true, true, false};
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "east");
    REQUIRE(hints.generalList() == 2);
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "west");
    REQUIRE(hints.next(HintTopic::General, knowledge, "").passages[0] == "first");
}

TEST_CASE("guardian and legend hints go through the open worlds still to be dealt with",
          "[game][world][hints]") {
    SumnerHints hints;
    REQUIRE(hints.load(sampleHints("hints-worlds")));
    HintKnowledge knowledge;
    // With nothing open the first guardian is all he speaks of.
    REQUIRE(hints.next(HintTopic::Guardians, knowledge, "").title == "title 1");
    REQUIRE(hints.guardian() == 1);
    hints.beginVisit();
    // Worlds 2 and 1 (the order's second and third) open, world 2's guardian beaten.
    knowledge.worldsOpen = worlds({2, 1});
    knowledge.guardiansBeaten = worlds({2});
    HintPage page = hints.next(HintTopic::Guardians, knowledge, "");
    REQUIRE(hints.guardian() == 3);
    REQUIRE(page.title == "title 3");
    REQUIRE(page.passages == std::vector<std::string>{"G3 one"});
    REQUIRE(page.scale == Approx(0.8f));
    REQUIRE_FALSE(page.centred);
    REQUIRE(page.gap == SumnerHints::kPassageGap);
    // Round again, the search widens to every open world, beaten or not.
    REQUIRE(hints.next(HintTopic::Guardians, knowledge, "").title == "title 2");
    REQUIRE(hints.next(HintTopic::Guardians, knowledge, "").title == "title 3");
    // Each try at a guardian earns a passage more.
    knowledge.guardianTries[0] = worlds({1});
    REQUIRE(hints.next(HintTopic::Guardians, knowledge, "").passages.size() == 1); // world 2
    REQUIRE(hints.next(HintTopic::Guardians, knowledge, "").passages.size() == 2);
    knowledge.guardianTries[1] = worlds({1});
    do {
        page = hints.next(HintTopic::Guardians, knowledge, "");
    } while (hints.guardian() != 3);
    REQUIRE(page.passages == std::vector<std::string>{"G3 one", "G3 two", "G3 three"});
    // The items of legend keep their own place and skip the ones found.
    knowledge.legendsFound = worlds({1});
    page = hints.next(HintTopic::Legends, knowledge, "");
    REQUIRE(hints.legend() == 2);
    REQUIRE(page.passages == std::vector<std::string>{"L2 one"});
}

TEST_CASE("runestone hints go through the stones still missing, in the original's order",
          "[game][world][hints]") {
    SumnerHints hints;
    REQUIRE(hints.load(sampleHints("hints-runes")));
    HintKnowledge knowledge;
    // The order starts with the eighth stone, then the ninth.
    HintPage page = hints.next(HintTopic::Runestones, knowledge, "");
    REQUIRE(hints.runestone() == 0);
    REQUIRE(page.title == "title 7");
    REQUIRE(page.passages == std::vector<std::string>{"R7 one"});
    REQUIRE(page.gap == 0);
    knowledge.runestonesFound = 1U << 8U; // the ninth is found: on to the fourth
    REQUIRE(hints.next(HintTopic::Runestones, knowledge, "").title == "title 3");
    hints.beginVisit();
    REQUIRE(hints.next(HintTopic::Runestones, knowledge, "").title == "title 7");
}

TEST_CASE("a party's crystals say which worlds are open", "[game][world][hints]") {
    REQUIRE(HintKnowledge::ofParty({}).worldsOpen == worlds({13}));
    std::vector<ClassProgress> party(2);
    party[1].crystals[1] = 15; // the first gate's, which opens world 7
    const HintKnowledge knowledge = HintKnowledge::ofParty(party);
    REQUIRE((knowledge.worldsOpen & worlds({7})) != 0);
    REQUIRE((knowledge.worldsOpen & worlds({2})) == 0);
    REQUIRE((knowledge.worldsOpen & worlds({5, 6, 8})) == 0); // those want runestones
    REQUIRE((knowledge.worldsOpen & worlds({13})) != 0);
}

TEST_CASE("the unpacked hints name the Lich first and tell of the green gas",
          "[game][world][hints][unpacked]") {
    SumnerHints hints;
    const auto path = test::unpackedOrSkip("text/hints_e.json");
    REQUIRE(hints.load(path));
    const HintPage general = hints.next(HintTopic::General, {}, "A Hint for You");
    REQUIRE(general.passages.size() == 1);
    REQUIRE(general.passages[0].starts_with("Your precious food"));
    const HintPage guardian = hints.next(HintTopic::Guardians, {}, "");
    REQUIRE(guardian.title == "The Lich");
    REQUIRE(guardian.passages.size() == 1);
    REQUIRE(guardian.passages[0].starts_with("The Book of Protection"));
    const HintPage stone = hints.next(HintTopic::Runestones, {}, "");
    REQUIRE(stone.title == "The First Runestone");
}

} // namespace
