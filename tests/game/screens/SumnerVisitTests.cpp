#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/SumnerVisit.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path sampleHints(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "hints.json", R"({
        "fonts": ["font32"],
        "messages": [{"name": "GENERAL", "font": 0, "scale": 1,
                      "lines": ["FIRST", "SECOND"]}],
        "lists": [{"name": "GENERAL_HINTS", "messages": [0]}]
    })");
    return dir / "hints.json";
}

BitmapFont font() {
    std::vector<BitmapGlyph> glyphs;
    for (std::int32_t c = 33; c < 127; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

struct Fixture {
    BitmapFont glyphs = font();
    test::FakeTexture sheet{64, 64};
    TextPainter text;
    GameConfig config;
    StringTable strings;
    test::FakeRenderDevice device;
    SumnerVisit visit;

    explicit Fixture(std::string_view name) {
        REQUIRE(visit.loadTexts(sampleHints(name)));
        text.setFont(&glyphs, &sheet);
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    }

    bool approach(float seconds, std::optional<std::int32_t> player = 0, bool ready = true) {
        return visit.visit(seconds, player, ready, text, &config, &strings);
    }

    HintMenuEvent step(const MenuInput& input = {}, std::int32_t ticks = 2) {
        const HintMenuEvent event = visit.update(device, input, ticks);
        if (event.kind == HintMenuEvent::Kind::Asked) {
            visit.answer(event.topic, text, &strings, {});
        }
        return event;
    }

    void open(std::int32_t player = 0) {
        REQUIRE(approach(0.0f, player));
        REQUIRE_FALSE(approach(SumnerVisit::kGreetingSeconds, player));
        REQUIRE(visit.active());
    }
};

TEST_CASE("Sumner greets once and hands the current visitor a localized scroll",
          "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-greeting");
    REQUIRE_FALSE(f.approach(0.0f, std::nullopt));
    REQUIRE_FALSE(f.approach(0.0f, 1, false));
    REQUIRE(f.approach(0.0f, 1));
    REQUIRE_FALSE(f.approach(1.0f, 1));
    REQUIRE_FALSE(f.visit.active());
    REQUIRE_FALSE(f.approach(1.0f, 2));
    REQUIRE(f.visit.owner() == 2);
    REQUIRE(f.visit.menu().topics().definition().playerLabel == "Player 3");
    REQUIRE(f.visit.menu().topics().definition().title == f.strings.get("hints.title"));
    REQUIRE_FALSE(f.approach(10.0f, 2));
}

TEST_CASE("Sumner's greeting counts down while the spot is empty",
          "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-absence");
    REQUIRE(f.approach(0.0f));
    REQUIRE_FALSE(f.approach(3.0f, std::nullopt));
    REQUIRE_FALSE(f.visit.active());
    REQUIRE_FALSE(f.approach(0.0f, 3));
    REQUIRE(f.visit.active());
    REQUIRE(f.visit.owner() == 3);
}

TEST_CASE("Sumner answers topics and emits navigation and departure cues",
          "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-menu");
    f.open();
    MenuInput input;
    input.down = true;
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Moved);
    REQUIRE(f.visit.menu().topics().selection() == 1);
    input = {};
    input.up = true;
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Moved);
    input = {};
    input.select = true;
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Asked);
    REQUIRE(f.visit.menu().reading());
    REQUIRE(f.visit.menu().page().definition().body == std::vector<std::string>{"FIRST"});
    input = {};
    input.back = true;
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Returned);
    REQUIRE_FALSE(f.visit.menu().reading());
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Left);
    for (std::int32_t i = 0; i < 100 && f.visit.active(); ++i) {
        f.step();
    }
    REQUIRE_FALSE(f.visit.active());
    REQUIRE(f.visit.owner() == -1);
    REQUIRE_FALSE(f.approach(3.0f)); // must leave the spot to get another scroll
    REQUIRE_FALSE(f.approach(0.0f, std::nullopt));
    f.open();
    input = {};
    input.select = true;
    REQUIRE(f.step(input).kind == HintMenuEvent::Kind::Asked);
    REQUIRE(f.visit.menu().page().definition().body == std::vector<std::string>{"SECOND"});
}

TEST_CASE("clearing a Sumner visit releases the menu and resets the greeting, not hint progress",
          "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-clear");
    f.open();
    MenuInput input;
    input.select = true;
    f.step(input);
    REQUIRE(f.visit.texts().generalHint() == 0);
    f.visit.clear();
    REQUIRE_FALSE(f.visit.active());
    REQUIRE(f.visit.owner() == -1);
    REQUIRE(f.visit.texts().generalHint() == 0);
    f.open(1);
    REQUIRE(f.visit.owner() == 1);
}

TEST_CASE("missing Sumner resources cannot open a scroll", "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-missing");
    SumnerVisit empty;
    REQUIRE_FALSE(empty.visit(0.0f, 0, true, f.text, &f.config, &f.strings));
    REQUIRE_FALSE(empty.visit(3.0f, 0, true, f.text, &f.config, &f.strings));
    REQUIRE_FALSE(empty.active());
    REQUIRE(f.approach(0.0f));
    REQUIRE_FALSE(f.visit.visit(3.0f, 0, true, f.text, nullptr, &f.strings));
    REQUIRE_FALSE(f.visit.active());
    REQUIRE_FALSE(f.approach(0.0f)); // failed opening still consumes this visit
    f.visit.clear();
    REQUIRE(f.approach(0.0f));
    REQUIRE_FALSE(f.visit.visit(3.0f, 0, true, f.text, &f.config, nullptr));
    REQUIRE_FALSE(f.visit.active());
}

} // namespace
