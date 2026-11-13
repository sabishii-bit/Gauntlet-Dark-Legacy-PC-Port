#include <algorithm>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/tower/SumnerVisit.h"
#include "game/tower/TowerGateways.h"
#include "game/widgets/DialoguePresentation.h"

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

TEST_CASE("Sumner ends his topics with his gateway row only when he offers his gateways",
          "[game][screens][sumner-visit][gateways]") {
    Fixture f("sumner-visit-gateways");
    f.open();
    CHECK(f.visit.menu().topics().definition().items.size() == 4); // the original's four alone
    f.visit.clear();
    REQUIRE(f.visit.visit(0.0f, 0, true, f.text, &f.config, &f.strings, true));
    REQUIRE_FALSE(
        f.visit.visit(SumnerVisit::kGreetingSeconds, 0, true, f.text, &f.config, &f.strings, true));
    const auto& items = f.visit.menu().topics().definition().items;
    REQUIRE(items.size() == 5);
    CHECK(items[0].code == 39); // the original's topics first, as they stand
    CHECK(items[4].text == "Legacy of Legends");
    CHECK(items[4].code == kGatewayTopic);
    CHECK(items[4].enabled);
    // Asked, the row is the scene's to answer: no page of hints opens for it.
    MenuInput down;
    down.down = true;
    for (s32 i = 0; i < 4; ++i) {
        REQUIRE(f.visit.update(f.device, down, 2).kind == HintMenuEvent::Kind::Moved);
    }
    MenuInput select;
    select.select = true;
    const HintMenuEvent asked = f.visit.update(f.device, select, 2);
    CHECK(asked.kind == HintMenuEvent::Kind::Asked);
    CHECK(asked.topic == kGatewayTopic);
    f.visit.answer(asked.topic, f.text, &f.strings, {});
    CHECK_FALSE(f.visit.menu().reading());
    // It answers with a page of choices, and a choice comes back with its code while the scroll
    // stays the visitor's until it has gone.
    std::vector<MenuItem> places{{"A", 0}, {"B", 1}};
    places[0].enabled = false;
    REQUIRE(f.visit.choose(places));
    CHECK(f.visit.menu().reading());
    const HintMenuEvent chosen = f.visit.update(f.device, select, 2);
    CHECK(chosen.kind == HintMenuEvent::Kind::Chose);
    CHECK(chosen.topic == 1);
    CHECK(f.visit.owner() == 0);
    for (s32 i = 0; i < 100 && f.visit.active(); ++i) {
        f.visit.update(f.device, {}, 2);
    }
    CHECK_FALSE(f.visit.active());
    CHECK(f.visit.owner() == -1);
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

TEST_CASE("imported Sumner topics keep six choices and release borrowed native art on clear",
          "[game][screens][sumner-visit][imported-dialogue]") {
    Fixture f("sumner-imported-topics");
    class NativePresentation final : public DialoguePresentation {
    public:
        explicit NativePresentation(const TextPainter& text) : m_text(text) {}
        const TextPainter& text() const override { return m_text; }
        f32 scale() const override { return 1; }
        s32 textWidth() const override { return 256; }
        usize linesPerPage() const override { return 8; }
        DialoguePage layout(std::string_view /*page*/, f32 /*scale*/,
                            std::string_view /*prompt*/) const override {
            return {};
        }
        void draw(Canvas& /*canvas*/, const DialoguePage& /*page*/, f32 /*scale*/,
                  std::string_view /*prompt*/, u8 /*promptAlpha*/) const override {}
        bool drawTopicsBackdrop(Canvas& /*canvas*/, const OptionMenu& /*menu*/,
                                std::string_view /*title*/) const override {
            ++draws;
            return false;
        }
        mutable usize draws = 0;

    private:
        const TextPainter& m_text;
    } native(f.text);
    HintMenuLabels labels;
    labels.title = "Sumner";
    labels.topics = {{"Continue", 0x11},  {"Shop", 0x15},       {"Rune Hint", 0x2e},
                     {"World Hint", 0xd}, {"Enemy Hint", 0x2f}, {"History", 2}};
    MenuDefinition definition;
    definition.showCursor = false;
    definition.backdrop = "native-scroll";
    definition.backdropX = 100;
    definition.backdropY = 70;
    definition.backdropWidth = 300;
    definition.backdropHeight = 230;
    for (s32 i = 0; i < 6; ++i) {
        definition.itemPositions.emplace_back(-256, 100 + i * 24);
    }
    test::FakeTexture scroll{32, 32};
    MenuTextures artwork;
    artwork.backdrop = &scroll;
    REQUIRE(f.visit.openTopics(2, f.text, {}, labels, &artwork, definition, &native));
    CHECK(f.visit.owner() == 2);
    CHECK(f.visit.menu().topics().definition().title.empty());
    const auto draw = [&] {
        f.device.draws.clear();
        Canvas canvas;
        canvas.begin(f.device, Mat4{1});
        f.visit.draw(canvas, f.text);
        canvas.end();
    };
    const auto before = test::liveDigest(f.visit);
    draw();
    CHECK(native.draws == 1);
    REQUIRE_FALSE(f.device.draws.empty());
    CHECK(f.device.draws.front().texture == &scroll);
    CHECK(test::liveDigest(f.visit) == before); // The borrowed draw callback is not tick state.
    for (usize i = 0; i < labels.topics.size(); ++i) {
        CAPTURE(i);
        MenuInput select;
        select.select = true;
        CHECK(f.visit.updateNative(f.device, 1, select, 1, 0, 0).kind ==
              HintMenuEvent::Kind::None); // Another seat cannot control this visit.
        const auto event = f.visit.update(f.device, select, 1);
        CHECK(event.kind == HintMenuEvent::Kind::Asked);
        CHECK(event.topic == labels.topics[i].code);
        if (i + 1 < labels.topics.size()) {
            MenuInput down;
            down.down = true;
            CHECK(f.visit.update(f.device, down, 1).kind == HintMenuEvent::Kind::Moved);
        }
    }
    for (usize i = 0; i < labels.topics.size(); ++i) {
        const Rect area = f.visit.menu().topics().itemArea(i);
        MenuInput click;
        click.pointer = Vec2{area.x + area.width / 2, area.y + area.height / 2};
        click.pointerPressed = true;
        const auto event = f.visit.update(f.device, click, 1);
        CHECK(event.kind == HintMenuEvent::Kind::Asked);
        CHECK(event.topic == labels.topics[i].code);
    }
    SumnerVisit::LocalSnapshot borrowed;
    f.visit.saveLocal(borrowed);
    f.visit.clear();
    CHECK_FALSE(f.visit.restoreLocal(borrowed));
    CHECK_FALSE(f.visit.active());
    f.open(0);
    CHECK(f.visit.menu().topics().definition().title == f.strings.get("hints.title"));
    CHECK(f.visit.menu().topics().definition().items.size() == 4);
    CHECK(f.visit.menu().topics().definition().backdrop == "SCROLL_A");
    f.visit.update(f.device, {}, 30);
    draw();
    CHECK(native.draws == 1); // No callback into the prior level's native presentation.
    CHECK(std::ranges::none_of(f.device.draws,
                               [&](const auto& item) { return item.texture == &scroll; }));
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

TEST_CASE("Sumner visit digest bytes stay pinned after the visit script and what it saved",
          "[game][screens][sumner-visit][digest-golden]") {
    Fixture f("sumner-visit-digest-golden");
    // The first player stands at the visit spot, pressing select at 12 and 17 and back at 15,
    // 20 and 22.
    const auto tick = [&f](s32 t) {
        const s32 phase = t % 80;
        const auto visitor = phase < 50 ? std::optional<s32>{(t / 80) % 4} : std::nullopt;
        f.approach(0.25f, visitor);
        MenuInput input;
        input.select = phase == 12 || phase == 17;
        input.back = phase == 15 || phase == 20 || phase == 22;
        f.step(input, 1);
    };
    for (s32 t = 0; t < 10; ++t) {
        tick(t);
    }
    const SumnerVisit& visit = f.visit;
    SumnerVisit::LocalSnapshot saved;
    HintMenu::LocalSnapshot menu;
    SumnerHints::LocalSnapshot hints;
    visit.saveLocal(saved);
    visit.menu().saveLocal(menu);
    visit.texts().saveLocal(hints);
    for (s32 t = 10; t < 24; ++t) {
        tick(t);
    }
    // SHA-256 of the visit's native digest 24 ticks in and of what it saved ten ticks in, before
    // the first select, which netplay peers must hash alike: a literal moves only when what is
    // visited changes on purpose. Its menu and hints follow it, so that a moved visit names the
    // child that moved it.
    test::checkPinnedDigests("SumnerVisit", test::liveDigest(visit),
                             test::savedDigest(visit, saved),
                             "36d70c6f554a7e1c30fe9c7906b2828a755a7a33ed363141c878bd72c113040d",
                             "5a9842ffd4187aa99cdc3edb274678c1ae36b7d15963c899935020d81abd716d");
    test::checkPinnedDigests("HintMenu", test::liveDigest(visit.menu()),
                             test::savedDigest(visit.menu(), menu),
                             "84a6e5858ef1e31ce9ed9135160767ed47cc0bbfa0f2a1ea6e4d00e50d0e099d",
                             "b13d952410061fab70309b68d6bed6bd3e333eb41ab7e8d00d852d6cb74b1c83");
    test::checkPinnedDigests("SumnerHints", test::liveDigest(visit.texts()),
                             test::savedDigest(visit.texts(), hints),
                             "0a81e5c2f49176de4f2c642c1046a36515c28129264a05c685630b8973263170",
                             "1b0b6721a07d5951f65ce040221c7b8580ee9e1d9bff8a93b6cfdf45b483a395");
}

} // namespace
