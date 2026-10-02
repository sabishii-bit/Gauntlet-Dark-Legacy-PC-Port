#include <array>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayScene.h"
#include "game/screens/RuneMeter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("runestone finder is earned, with any stone owner overriding permission",
          "[rune-meter]") {
    std::array<PartyMember, 2> party{};
    CHECK_FALSE(RuneMeter::eligible(0, 7, party));
    party[1].save.progress().relics.addShard(9);
    CHECK(RuneMeter::eligible(0, 7, party));
    CHECK_FALSE(RuneMeter::eligible(0, 8, party));
    party[0].save.progress().levels.recordBeaten(8, 2);
    CHECK(RuneMeter::eligible(0, 8, party));
    party[0].save.progress().relics.addRune(0);
    CHECK_FALSE(RuneMeter::eligible(0, 7, party));
    CHECK_FALSE(RuneMeter::eligible(0, 8, party));
    std::swap(party[0], party[1]);
    CHECK_FALSE(RuneMeter::eligible(0, 7, party));
    CHECK_FALSE(RuneMeter::eligible(-1, 7, party));
    CHECK_FALSE(RuneMeter::eligible(13, 7, party));
    CHECK_FALSE(RuneMeter::eligible(1, 7, {}));
}

TEST_CASE("runestone proximity measures the camera point and emits each line once",
          "[rune-meter]") {
    std::array<PartyMember, 1> party{};
    party[0].save.progress().relics.addShard(9);
    RuneMeter meter;
    meter.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, party);
    REQUIRE(meter.visible());
    CHECK(meter.update(Vec3{100, 0, 0}, true) == RuneMeter::Cue::None);
    CHECK(meter.fill() == 0);
    CHECK(meter.update(Vec3{16, 0, 0}, true) == RuneMeter::Cue::Closer);
    CHECK(meter.update(Vec3{16, 0, 0}, true) == RuneMeter::Cue::None);
    CHECK(meter.update(Vec3{8, 0, 0}, true) == RuneMeter::Cue::Nearby);
    CHECK(meter.fill() == 1);
    CHECK(meter.update(Vec3{100, 0, 0}, true) == RuneMeter::Cue::None);
    CHECK(meter.update(Vec3{0}, true) == RuneMeter::Cue::None);
    CHECK(meter.update(Vec3{0}, false) == RuneMeter::Cue::None);
    CHECK_FALSE(meter.visible());
    CHECK(meter.update(Vec3{0}, true) == RuneMeter::Cue::None);
    meter.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, party);
    CHECK(meter.update(Vec3{0}, true) == RuneMeter::Cue::Closer);
    CHECK(meter.update(Vec3{0}, true) == RuneMeter::Cue::Nearby);
    meter.clear();
    CHECK_FALSE(meter.visible());
    meter.begin(0, Vec3{0}, Vec3{0}, 7, party);
    CHECK_FALSE(meter.visible());
}

TEST_CASE("runestone thermometer uses the retail nonlinear fill and crop", "[rune-meter]") {
    CHECK(RuneMeter::closeness(43, 100) == Approx(0.25f));
    CHECK(RuneMeter::closeness(78, 100) == 0);
    CHECK(RuneMeter::closeness(0, 100) == 1);
    CHECK(RuneMeter::closeness(10, 0) == 0);
    CHECK(RuneMeter::closeness(10, std::numeric_limits<f32>::infinity()) == 0);
    const auto empty = RuneMeter::column(0, 128);
    CHECK(empty.area.x == 392);
    CHECK(empty.area.y == 102);
    CHECK(empty.area.height == 27);
    CHECK(empty.uv.y == Approx(101.0f / 128));
    const auto full = RuneMeter::column(1, 128);
    CHECK(full.area.y == 29);
    CHECK(full.area.height == 100);
    CHECK(full.uv.y == Approx(28.0f / 128));
}

TEST_CASE("runestone finder draws both retail textures and nothing after collection",
          "[rune-meter][assets]") {
    const auto directory = test::assetOrSkip("STATIC/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    TextureSet textures;
    REQUIRE(textures.load(directory));
    const auto frame = textures.find("THERMBASE");
    const auto column = textures.find("THERMCOL");
    REQUIRE(frame);
    REQUIRE(column);
    const auto& frameTexture = textures.texture(device, *frame);
    const auto& columnTexture = textures.texture(device, *column);
    std::array<PartyMember, 1> party{};
    party[0].save.progress().relics.addShard(9);
    RuneMeter meter;
    meter.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, party);
    meter.update(Vec3{0}, true);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    meter.draw(canvas, frameTexture, columnTexture);
    canvas.end();
    CHECK(device.draws.size() == 2);
    device.draws.clear();
    meter.update(Vec3{0}, false);
    canvas.begin(device, Mat4{1});
    meter.draw(canvas, frameTexture, columnTexture);
    canvas.end();
    CHECK(device.draws.empty());
}
TEST_CASE("the level scene binds its authored runestone and stops the finder when it goes",
          "[rune-meter][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("G1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    context.levels = &catalog;
    std::array<PartyMember, 1> party{};
    party[0].save.progress().relics.addShard(9);
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party));
    REQUIRE(scene.runeMeter().visible());
    std::optional<usize> stone;
    for (usize i = 0; i < world.placedItems().size(); ++i) {
        if (world.placedItems().item(i).subtype == ItemInfo::kRunestone) {
            stone = i;
        }
    }
    REQUIRE(stone);
    for (s32 frame = 0; frame < 240; ++frame) {
        scene.update(1.0 / 30, {});
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE(scene.runeMeter().visible());
    world.discardItem(*stone);
    scene.update(1.0 / 30, {});
    CHECK_FALSE(scene.runeMeter().visible());
    scene.close();
    CHECK_FALSE(scene.runeMeter().visible());
}
} // namespace
