#include <array>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelLoadingScreen.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("encounter movies play for newcomers and remain seen across save reloads",
          "[level-loading][save]") {
    std::array<PartyMember, 2> party;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    LevelLoadingScreen::rememberMovie("movieG1", party);
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", party));
    LevelLoadingScreen::rememberMovie("movieG1", party);
    CHECK(party[0].save.moviesSeen.size() == 1);
    party[0].save = CharacterSave::fromJson(party[0].save.toJson());
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG4", party));
    party[1].save = {};
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[1].fallen = true;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", party));
}

TEST_CASE("travel keeps a map before the stage preview and skips both for tower returns",
          "[level-loading][unpacked]") {
    const auto root = test::unpackedOrSkip("MAPS/LEVELG1/textures.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("wdata/TOWN.json");
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelLoadingScreen screen;
    screen.open(context, *level);
    CHECK(screen.active());
    CHECK(screen.movie() == "movieG1");
    CHECK(screen.previewAlpha() == 0);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    screen.draw(canvas, device, 512);
    canvas.end();
    REQUIRE(device.draws.size() >= 5);
    CHECK_FALSE(screen.update(3.5f));
    CHECK(screen.previewAlpha() == 0);
    CHECK_FALSE(screen.update(1.5f));
    CHECK(screen.previewAlpha() == 1);
    CHECK(screen.update(2));
    screen.close();
    CHECK_FALSE(screen.active());
    screen.open(context, LevelRef::tower());
    CHECK_FALSE(screen.active());
    CHECK(screen.update(0));
    CHECK(screen.movie().empty());
}
} // namespace
