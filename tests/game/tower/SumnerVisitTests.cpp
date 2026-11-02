#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/tower/SumnerVisit.h"

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
    for (s32 c = 33; c < 127; ++c) {
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

    bool approach(f32 seconds, std::optional<s32> player = 0, bool ready = true) {
        return visit.visit(seconds, player, ready, text, &config, &strings);
    }

    HintMenuEvent step(const MenuInput& input = {}, s32 ticks = 2) {
        const HintMenuEvent event = visit.update(device, input, ticks);
        if (event.kind == HintMenuEvent::Kind::Asked) {
            visit.answer(event.topic, text, &strings, {});
        }
        return event;
    }

    void open(s32 player = 0) {
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
    for (s32 i = 0; i < 100 && f.visit.active(); ++i) {
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

TEST_CASE("a Sumner scroll goes with an owner who leaves play", "[game][screens][sumner-visit]") {
    Fixture f("sumner-visit-owner-gone");
    f.open(3);
    f.visit.release(1);
    REQUIRE(f.visit.active());
    REQUIRE(f.visit.owner() == 3);
    f.visit.release(3);
    CHECK_FALSE(f.visit.active());
    CHECK(f.visit.owner() == -1);
    REQUIRE_FALSE(f.approach(0.0f, std::nullopt));
    f.open(1); // the next visitor is greeted anew
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

TEST_CASE("native Sumner visit rewind restores greeting ownership hint progression and cues",
          "[game][screens][sumner-visit][sumner-visit-local-rewind]") {
    Fixture expected("sumner-local-reference");
    Fixture corrected("sumner-local-corrected");
    struct Cue {
        bool greeting = false;
        HintMenuEvent menu;
    };
    const auto tick = [](Fixture& f, s32 t) {
        const s32 phase = t % 80;
        const auto visitor = phase < 50 ? std::optional<s32>{(t / 80) % 4} : std::nullopt;
        const bool greeting = f.approach(0.25f, visitor);
        MenuInput input;
        input.select = phase == 12 || phase == 17;
        input.back = phase == 15 || phase == 20 || phase == 22;
        return Cue{greeting, f.step(input, 1)};
    };
    s32 at = 0;
    s32 greetings = 0;
    s32 questions = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 delay = 1 + window % 8;
        CAPTURE(window, at, delay);
        SumnerVisit::LocalSnapshot saved;
        corrected.visit.saveLocal(saved);
        REQUIRE_FALSE(expected.visit.restoreLocal(saved));
        std::vector<Cue> cues;
        for (s32 t = at; t < at + delay; ++t) {
            cues.push_back(tick(expected, t));
            corrected.approach(3, std::nullopt);
            corrected.approach(3, 3);
            corrected.visit.answer(39, corrected.text, &corrected.strings, {});
            MenuInput wrong;
            wrong.back = true;
            corrected.step(wrong, 11);
        }
        const auto created = corrected.device.texturesCreated;
        const auto uploaded = corrected.device.textureUpdates;
        REQUIRE(corrected.visit.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            const auto actual = tick(corrected, t);
            const auto& cue = cues[static_cast<usize>(t - at)];
            CHECK(actual.greeting == cue.greeting);
            CHECK(actual.menu.kind == cue.menu.kind);
            CHECK(actual.menu.topic == cue.menu.topic);
            greetings += actual.greeting ? 1 : 0;
            questions += actual.menu.kind == HintMenuEvent::Kind::Asked ? 1 : 0;
        }
        CHECK(corrected.device.texturesCreated == created);
        CHECK(corrected.device.textureUpdates == uploaded);
        CHECK(corrected.visit.owner() == expected.visit.owner());
        CHECK(corrected.visit.active() == expected.visit.active());
        CHECK(corrected.visit.menu().revision() == expected.visit.menu().revision());
        CHECK(corrected.visit.menu().reading() == expected.visit.menu().reading());
        CHECK(corrected.visit.menu().page().definition().body ==
              expected.visit.menu().page().definition().body);
        CHECK(corrected.visit.texts().generalHint() == expected.visit.texts().generalHint());
        const auto draw = [](Fixture& f) {
            f.visit.prepare(f.device);
            f.device.draws.clear();
            Canvas canvas;
            canvas.begin(f.device, Mat4{1});
            f.visit.draw(canvas, f.text);
            canvas.end();
        };
        draw(expected);
        draw(corrected);
        REQUIRE(corrected.device.draws.size() == expected.device.draws.size());
        for (usize i = 0; i < expected.device.draws.size(); ++i) {
            test::checkRecordedDraw(corrected.device.draws[i], expected.device.draws[i]);
        }
        at += delay;
    }
    CHECK(greetings >= 5);
    CHECK(questions >= 10);
    SumnerVisit::LocalSnapshot stale;
    corrected.visit.saveLocal(stale);
    corrected.visit.clear();
    CHECK_FALSE(corrected.visit.restoreLocal(stale));
    CHECK(corrected.visit.owner() == -1);
}

TEST_CASE("Sumner's scroll keeps each machine's field of view out of the shared state",
          "[game][screens][sumner-visit][sumner-visit-fov]") {
    Fixture narrow("sumner-visit-fov-narrow");
    Fixture wide("sumner-visit-fov-wide");
    narrow.config.camera.horizontalFovDegrees = 42.0f;
    wide.config.camera.horizontalFovDegrees = 97.0f;
    const auto shared = [](const Fixture& f) {
        NativeStateDigest digest;
        REQUIRE(f.visit.appendNativeDigest(digest));
        return digest.finish();
    };
    const auto rewound = [](const Fixture& f) {
        SumnerVisit::LocalSnapshot saved;
        f.visit.saveLocal(saved);
        NativeStateDigest digest;
        REQUIRE(f.visit.appendLocalDigest(digest, saved));
        return digest.finish();
    };
    narrow.open(3);
    wide.open(3);
    // Each machine still sizes the scroll's arrow for its own camera.
    CHECK(narrow.visit.menu().topics().iconScale() > wide.visit.menu().topics().iconScale());
    CHECK(shared(narrow) == shared(wide));
    CHECK(rewound(narrow) == rewound(wide));
    MenuInput select;
    select.select = true;
    MenuInput back;
    back.back = true;
    // Asked and answered, back to the topics, then away: the same shared state throughout.
    for (const MenuInput& input : {select, back, back}) {
        const auto kind = narrow.step(input).kind;
        CHECK(wide.step(input).kind == kind);
        CHECK(shared(narrow) == shared(wide));
        CHECK(rewound(narrow) == rewound(wide));
    }
    CHECK_FALSE(narrow.visit.menu().reading());
    CHECK(narrow.visit.active() == wide.visit.active());
}

} // namespace
