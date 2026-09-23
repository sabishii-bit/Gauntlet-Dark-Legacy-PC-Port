#include <cstddef>
#include <cstdint>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelMessages.h"

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
    for (std::size_t i = 0; i < names.size(); ++i) {
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
        {"name": "EMPTY", "font": 0, "scale": 1, "lines": []}], "lists": []})");
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
            std::int32_t endings = 0;
            for (std::int32_t i = 0; i < 300 && f.messages.active(); ++i) {
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

} // namespace
