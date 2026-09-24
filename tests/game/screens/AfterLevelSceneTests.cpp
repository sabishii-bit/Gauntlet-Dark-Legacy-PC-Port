#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/AfterLevelScene.h"
namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("after-level screen fails safely without its portable catalog", "[shop][screens]") {
    test::FakeRenderDevice device;
    AfterLevelScene scene;
    GameContext context;
    context.unpackedRoot = test::scratchDirectory("shop-screen-empty");
    REQUIRE_FALSE(scene.open(device, context, {}, {}, {}, "G1"));
    REQUIRE_FALSE(scene.isOpen());
    scene.close();
    scene.close();
}
TEST_CASE("after-level screen renders every phase with retail assets",
          "[shop][screens][unpacked]") {
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    test::unpackedOrSkip("SELECT/textures.json");
    test::unpackedOrSkip("INVENTORY/textures.json");
    test::unpackedOrSkip("pdata/WAR.json");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    AfterLevelScene scene;
    CharacterSave save;
    save.gold = 5000;
    save.name = "SHOP";
    save.progress().health = 100;
    const std::array<PartyMember, 1> party{{{2, save}}};
    const std::array<LevelResults, 1> results{{{2, {500, 40, 900}}}};
    REQUIRE(scene.open(device, context, party, results, {1000, 100, 1000}, "G1"));
    REQUIRE(scene.isOpen());
    const Mat4 projection{1};
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(scene.update(10, {}));
    ShopSession::Inputs input;
    input[2].select = true;
    REQUIRE_FALSE(scene.update(0, input));
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(scene.update(0, input));
    scene.render(device, projection, 512, 384);
    // Walk every catalog entry: this also exercises all optional artwork and text wrapping.
    input = {};
    input[2].down = true;
    for (usize i = 0; i < 34; ++i) {
        REQUIRE_FALSE(scene.update(0, input));
        scene.render(device, projection, 512, 384);
    }
    input = {};
    input[2].select = true;
    REQUIRE_FALSE(scene.update(0, input));
    scene.render(device, projection, 512, 384);
    REQUIRE(scene.update(0, input));
    scene.render(device, projection, 512, 384);
    REQUIRE(scene.session().party()[0].save.gold == 5000);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}
} // namespace
