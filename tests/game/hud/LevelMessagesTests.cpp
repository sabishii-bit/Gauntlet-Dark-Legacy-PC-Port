#include <algorithm>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/hud/LevelMessages.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path sampleRoot(std::string_view name, bool burn) {
    const auto root = test::scratchDirectory(name);
    std::filesystem::create_directories(root / "STATIC");
    std::filesystem::create_directories(root / "fonts");
    std::filesystem::create_directories(root / "text");
    writeFile(root / "STATIC/tile.png", test::kTinyPng);
    std::vector<std::string_view> names{"FONT32", "SCROLL_A", "FONT32_GLOW", "BUTTON_TRI"};
    if (burn) {
        names.insert(names.end(), {"GREENCIRCTRANS", "GREENCIRCTRANS+1", "GREENCIRCTRANSM",
                                   "GREENCIRCTRANSM+1"});
    }
    std::string manifest = R"({"bitmaps": [)";
    for (usize i = 0; i < names.size(); ++i) {
        if (i != 0) {
            manifest += ',';
        }
        manifest += std::format(R"({{"index": {}, "name": "{}", "file": "tile.png",
                                   "width": 2, "height": 2}})",
                                i, names[i]);
    }
    writeTextFile(root / "STATIC/textures.json", manifest + "]}");
    writeTextFile(root / "fonts/font32.json", R"({"height": 10, "glyphs": [
        {"code": 65, "width": 6, "x": 0, "y": 0},
        {"code": 66, "width": 8, "x": 0, "y": 0}]})");
    writeTextFile(root / "text/scroll_e.json", R"({"fonts": ["font32"], "messages": [
        {"name": "WELCOME", "font": 0, "scale": 1, "lines": ["A", "B"]},
        {"name": "EMPTY", "font": 0, "scale": 1, "lines": []},
        {"name": "SMALL", "font": 0, "scale": 0.5, "lines": ["A"]}], "lists": []})");
    return root;
}

struct Fixture {
    std::filesystem::path root;
    test::FakeRenderDevice device;
    TextureSet textures;
    LevelMessages messages;

    explicit Fixture(std::string_view name, bool burn = false) : root(sampleRoot(name, burn)) {
        messages.load(device, textures, root, nullptr);
    }

    LevelMessages::Cues accept() {
        messages.step(ScrollBox::kHoldTicks, 0);
        return messages.step(1, 1);
    }
};

TEST_CASE("level messages load artwork and show an entire message or one selected page",
          "[game][screens][level-messages]") {
    Fixture f("level-messages-pages");
    REQUIRE(f.messages.text().ready());
    REQUIRE(f.messages.open(f.device, "WELCOME", nullptr));
    REQUIRE(f.messages.scroll().pageCount() == 2);
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"A"});
    REQUIRE_FALSE(f.accept().stopVoice);
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"B"});
    REQUIRE(f.messages.open(f.device, "WELCOME", nullptr, 1));
    REQUIRE(f.messages.scroll().pageCount() == 1);
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"B"});
    Canvas canvas;
    canvas.begin(f.device, Mat4{1.0f});
    f.messages.draw(canvas);
    canvas.end();
    REQUIRE_FALSE(f.device.draws.empty());
    const auto backdrop = f.textures.find("SCROLL_A");
    REQUIRE(backdrop.has_value());
    REQUIRE(f.device.draws.front().texture == &f.textures.texture(f.device, *backdrop));
}

TEST_CASE("invalid level message lookups leave the current scroll alone",
          "[game][screens][level-messages]") {
    Fixture f("level-messages-invalid");
    REQUIRE(f.messages.open(f.device, "WELCOME", nullptr, 1));
    REQUIRE_FALSE(f.messages.open(f.device, "UNKNOWN", nullptr));
    REQUIRE_FALSE(f.messages.open(f.device, "UNKNOWN", nullptr, 0));
    REQUIRE_FALSE(f.messages.open(f.device, "WELCOME", nullptr, 2));
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"B"});
    REQUIRE(f.messages.active());
    REQUIRE_FALSE(f.messages.open(f.device, "EMPTY", nullptr));
    REQUIRE_FALSE(f.messages.active());
}

TEST_CASE("level scroll dismissal reports audio cues only at its boundaries",
          "[game][screens][level-messages]") {
    for (const bool burn : {false, true}) {
        Fixture f(burn ? "level-messages-burn" : "level-messages-close", burn);
        REQUIRE_FALSE(f.messages.step(1, 1).stopVoice);
        REQUIRE(f.messages.open(f.device, "WELCOME", nullptr));
        const auto held = f.messages.step(1, 1);
        REQUIRE_FALSE(held.stopVoice);
        REQUIRE_FALSE(held.burnSound);
        const auto page = f.accept();
        REQUIRE_FALSE(page.stopVoice);
        REQUIRE_FALSE(page.burnSound);
        const auto dismissed = f.accept();
        REQUIRE(dismissed.stopVoice);
        REQUIRE(dismissed.burnSound);
        REQUIRE(f.messages.active() == burn);
        if (burn) {
            REQUIRE(f.messages.scroll().burning());
            f.messages.prepare(f.device);
            s32 endings = 0;
            for (s32 i = 0; i < 300 && f.messages.active(); ++i) {
                const auto cue = f.messages.step(1, 1);
                REQUIRE_FALSE(cue.burnSound);
                if (cue.stopVoice) {
                    ++endings;
                }
            }
            REQUIRE(endings == 1);
            REQUIRE_FALSE(f.messages.active());
        }
        const auto idle = f.messages.step(1, 1);
        REQUIRE_FALSE(idle.stopVoice);
        REQUIRE_FALSE(idle.burnSound);
    }
}

TEST_CASE("level messages translate pages and clear their borrowed rendering state",
          "[game][screens][level-messages]") {
    Fixture f("level-messages-language");
    writeTextFile(f.root / "text/test.json",
                  R"({"scroll.welcome.1": "B", "scroll.welcome.2": "A",
                      "scroll.pressButton": "AB"})");
    StringTable strings;
    REQUIRE(strings.load(f.root / "text", "test", "test"));
    f.messages.clear();
    f.messages.load(f.device, f.textures, f.root, &strings);
    REQUIRE(f.messages.open(f.device, "WELCOME", &strings));
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"B"});
    Canvas promptCanvas;
    promptCanvas.begin(f.device, Mat4{1});
    f.messages.draw(promptCanvas);
    promptCanvas.end();
    const auto font = f.textures.find("FONT32");
    REQUIRE(font);
    test::FakeRenderDevice expected;
    Canvas expectedCanvas;
    expectedCanvas.begin(expected, Mat4{1});
    const s32 promptY = static_cast<s32>(f.messages.scroll().area().y) + ScrollBox::kTextInset +
                        f.messages.text().lineHeight(1) + ScrollBox::kPromptGap;
    f.messages.text().draw(expectedCanvas, -ScrollBox::kCentreX, promptY, "AB",
                           TextStyle{ScrollBox::kPromptScale, Color::white()});
    expectedCanvas.end();
    REQUIRE_FALSE(expected.draws.empty());
    CHECK(std::ranges::any_of(f.device.draws, [&](const auto& draw) {
        return draw.texture == &f.textures.texture(f.device, *font) &&
               !std::ranges::search(draw.vertices, expected.draws.front().vertices).empty();
    }));
    f.device.draws.clear();
    f.messages.clear();
    REQUIRE_FALSE(f.messages.active());
    REQUIRE_FALSE(f.messages.text().ready());
    REQUIRE_FALSE(f.messages.open(f.device, "WELCOME", &strings));
    f.textures.releaseTextures();
    f.messages.prepare(f.device);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1.0f});
    f.messages.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws.empty());
    f.messages.load(f.device, f.textures, f.root, nullptr);
    REQUIRE(f.messages.open(f.device, "WELCOME", nullptr));
    REQUIRE(f.messages.scroll().lines() == std::vector<std::string>{"A"});
}

TEST_CASE("the game's own pages open drawn as a named message of the rom's is",
          "[game][screens][level-messages]") {
    Fixture f("level-messages-styled");
    REQUIRE(f.messages.text().ready());
    writeTextFile(f.root / "text/test.json", R"({"scroll.pressButton": "AB"})");
    StringTable strings;
    REQUIRE(strings.load(f.root / "text", "test", "test"));
    // The pages keep their lines, at that message's scale, with the prompt under them and no
    // message of the rom's named.
    REQUIRE(f.messages.openStyled(f.device, "SMALL", {"AB\nB", "A"}, &strings));
    const ScrollBox& scroll = f.messages.scroll();
    CHECK(scroll.pageCount() == 2);
    CHECK(scroll.lines() == std::vector<std::string>{"AB", "B"});
    CHECK(scroll.area() == scroll.layout("AB\nB", 0.5f, "AB").area);
    CHECK_FALSE(scroll.area() == scroll.layout("AB\nB", 1.0f, "AB").area);
    CHECK_FALSE(f.messages.look());
    // It never cuts another scroll short.
    CHECK_FALSE(f.messages.openStyled(f.device, "WELCOME", {"A"}, &strings));
    CHECK(scroll.lines() == std::vector<std::string>{"AB", "B"});
    // Each page holds as the rom's do: no button for its first fifteen ticks.
    f.messages.step(ScrollBox::kHoldTicks, 1);
    CHECK(scroll.page() == 0);
    f.messages.step(1, 1);
    CHECK(scroll.page() == 1);
    CHECK(scroll.lines() == std::vector<std::string>{"A"});
    f.accept();
    CHECK_FALSE(f.messages.active());
    // Without the message, a page or the scroll's text, nothing opens.
    CHECK_FALSE(f.messages.openStyled(f.device, "UNKNOWN", {"A"}, &strings));
    CHECK_FALSE(f.messages.openStyled(f.device, "SMALL", {}, &strings));
    LevelMessages unloaded;
    CHECK_FALSE(unloaded.openStyled(f.device, "SMALL", {"A"}, &strings));
    CHECK_FALSE(f.messages.active());
    CHECK_FALSE(unloaded.active());
}

TEST_CASE("scroll continuation uses plain device independent localized text",
          "[game][screens][level-messages]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    CHECK(strings.get("scroll.pressButton") == "Press any button to continue");
}

TEST_CASE("the native scroll font fits the complete continuation prompt inside its parchment",
          "[game][screens][level-messages][assets]") {
    const auto root = test::assetOrSkip("FONTS/font32.fnt").parent_path().parent_path();
    test::FakeRenderDevice device;
    TextureSet textures;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LevelMessages messages;
    messages.load(device, textures, root, &strings);
    REQUIRE(messages.open(device, "WELCOMEMESSAGE", &strings));
    const s32 width =
        messages.text().measure(strings.get("scroll.pressButton"), ScrollBox::kPromptScale);
    CHECK(width > 0);
    CHECK(width + ScrollBox::kPromptExtra <= messages.scroll().area().width);
    CHECK(messages.scroll().area().width <= ScrollBox::kMaxWidth);
}

TEST_CASE("missing level message resources disable presentation without failing the scene",
          "[game][screens][level-messages]") {
    Fixture f("level-messages-missing");
    f.messages.clear();
    std::filesystem::remove(f.root / "STATIC/tile.png");
    f.messages.load(f.device, f.textures, f.root, nullptr);
    REQUIRE_FALSE(f.messages.text().ready());
    REQUIRE_FALSE(f.messages.open(f.device, "WELCOME", nullptr));
    f.messages.clear();
    std::filesystem::remove(f.root / "fonts/font32.json");
    f.messages.load(f.device, f.textures, f.root, nullptr);
    REQUIRE_FALSE(f.messages.text().ready());
    REQUIRE_FALSE(f.messages.active());
}

TEST_CASE("native level scroll rewind replays page and dismissal cues exactly once",
          "[game][screens][level-messages][level-messages-local-rewind]") {
    Fixture expected("level-messages-local-reference", true);
    Fixture corrected("level-messages-local-corrected", true);
    const auto tick = [](Fixture& f, s32 t) {
        if (t % 100 == 0) {
            REQUIRE(f.messages.open(f.device, "WELCOME", nullptr));
        }
        return f.messages.step(1, t % 5 == 0 ? 2U : 0U);
    };
    s32 at = 0;
    s32 burns = 0;
    s32 stops = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 delay = 1 + window % 8;
        CAPTURE(window, at, delay);
        LevelMessages::LocalSnapshot saved;
        corrected.messages.saveLocal(saved);
        REQUIRE_FALSE(expected.messages.restoreLocal(saved));
        std::vector<LevelMessages::Cues> cues;
        for (s32 t = at; t < at + delay; ++t) {
            cues.push_back(tick(expected, t));
            REQUIRE(corrected.messages.open(corrected.device, "WELCOME", nullptr, 1));
            corrected.messages.step(15, 0);
            corrected.messages.step(1, 1);
            corrected.messages.prepare(corrected.device);
            corrected.messages.step(40, 1);
        }
        const auto created = corrected.device.texturesCreated;
        const auto uploaded = corrected.device.textureUpdates;
        REQUIRE(corrected.messages.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            const auto actual = tick(corrected, t);
            const auto& cue = cues[static_cast<usize>(t - at)];
            CHECK(actual.stopVoice == cue.stopVoice);
            CHECK(actual.burnSound == cue.burnSound);
            burns += actual.burnSound ? 1 : 0;
            stops += actual.stopVoice ? 1 : 0;
        }
        CHECK(corrected.device.texturesCreated == created);
        CHECK(corrected.device.textureUpdates == uploaded);
        const auto a = corrected.messages.look();
        const auto b = expected.messages.look();
        REQUIRE(a.has_value() == b.has_value());
        if (a) {
            CHECK(a->message == b->message);
            CHECK(a->page == b->page);
            CHECK(a->burnFrame == b->burnFrame);
            CHECK(a->promptAlpha == b->promptAlpha);
        }
        CHECK(corrected.messages.scroll().lines() == expected.messages.scroll().lines());
        const auto draw = [](Fixture& f) {
            f.messages.prepare(f.device);
            f.device.draws.clear();
            Canvas canvas;
            canvas.begin(f.device, Mat4{1});
            f.messages.draw(canvas);
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
    CHECK(burns >= 4);
    CHECK(stops >= 8);
    LevelMessages::LocalSnapshot stale;
    corrected.messages.saveLocal(stale);
    corrected.messages.clear();
    CHECK_FALSE(corrected.messages.restoreLocal(stale));
    CHECK_FALSE(corrected.messages.active());
}

} // namespace
