#include <array>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelLoadingScreen.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("encounter movies play on every entry, including characters with legacy seen flags",
          "[level-loading][save]") {
    std::array<PartyMember, 2> party;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].save.moviesSeen.emplace_back("movieG1");
    party[1].save.moviesSeen.emplace_back("movieG1");
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].save = CharacterSave::fromJson(party[0].save.toJson());
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG4", party));
    party[1].save = {};
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[1].fallen = true;
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].fallen = true;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", party));
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", {}));
}

TEST_CASE("travel keeps a map before the stage preview and skips both for tower returns",
          "[level-loading][assets]") {
    const auto root =
        test::assetOrSkip("MAPS/LEVELG1/textures.ngc").parent_path().parent_path().parent_path();
    test::assetOrSkip("WDATA/TOWN.WAD");
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelLoadingScreen screen;
    screen.open(device, context, *level);
    CHECK(screen.active());
    CHECK(screen.movie() == "movieG1");
    CHECK(screen.previewAlpha() == 0);
    Canvas canvas;
    canvas.begin(device, makeLetterboxProjection(512, 384, 1920, 1080));
    screen.draw(canvas, device);
    canvas.end();
    REQUIRE(device.draws.size() >= 5);
    CHECK(device.draws.front().transform == Mat4{1});
    CHECK(test::minCorner(device.draws.front()) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws.front()) == Vec2{1, 1});
    CHECK(device.draws.front().vertices.front().color == Color::black());
    std::array<bool, 4> panels{};
    for (const auto& draw : device.draws) {
        if (test::minCorner(draw).y == StatusBoxPainter::kY &&
            test::maxCorner(draw).y == StatusBoxPainter::kY + StatusBoxPainter::kHeight &&
            test::maxCorner(draw).x - test::minCorner(draw).x == StatusBoxPainter::kWidth) {
            const auto slot =
                static_cast<usize>(test::minCorner(draw).x / StatusBoxPainter::kWidth);
            REQUIRE(slot < panels.size());
            panels[slot] = true;
        }
    }
    CHECK(panels == std::array{true, true, true, true});
    CHECK_FALSE(screen.update(3.5f));
    CHECK(screen.previewAlpha() == 0);
    CHECK_FALSE(screen.update(1.5f));
    CHECK(screen.previewAlpha() == 1);
    CHECK(screen.update(2));
    screen.close();
    CHECK_FALSE(screen.active());
    screen.open(device, context, LevelRef::tower());
    CHECK_FALSE(screen.active());
    CHECK(screen.update(0));
    CHECK(screen.movie().empty());
}
} // namespace
