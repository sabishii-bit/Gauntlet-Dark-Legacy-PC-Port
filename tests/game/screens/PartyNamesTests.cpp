#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("a name over the head is the save's first six letters, spaces for underscores",
          "[game][screens][names]") {
    CHECK(PartyNames::shownName("").empty());
    CHECK(PartyNames::shownName("AB") == "AB");
    CHECK(PartyNames::shownName("JO_ANNE") == "JO ANN");
    CHECK(PartyNames::shownName("ABCDEFGH") == "ABCDEF");
}

TEST_CASE("a point in front of the eye lands on the canvas; one behind it does not",
          "[game][screens][names]") {
    const Mat4 clip{1.0f}; // clip space is the world here: w is always one
    const auto middle = PartyNames::screenOf(clip, Vec3{0.0f}, 512.0f, 384.0f);
    REQUIRE(middle.has_value());
    CHECK(middle->x == Approx(256.0f));
    CHECK(middle->y == Approx(192.0f));
    const auto corner = PartyNames::screenOf(clip, Vec3{1.0f, 1.0f, 0.0f}, 512.0f, 384.0f);
    REQUIRE(corner.has_value());
    CHECK(corner->x == Approx(512.0f));
    CHECK(corner->y == Approx(0.0f));
    Mat4 behind{1.0f};
    behind[3][3] = -1.0f;
    CHECK_FALSE(PartyNames::screenOf(behind, Vec3{0.0f}, 512.0f, 384.0f).has_value());
}

TEST_CASE("names show for 240 ticks as a level opens, held while play is held",
          "[game][screens][names]") {
    std::array<PlayerRuntime, 2> players;
    PartyNames::show(players);
    CHECK(players[0].nameTicks == PartyNames::kTicks);
    PartyNames names;
    names.step(players, 100, true);
    CHECK(players[0].nameTicks == PartyNames::kTicks);
    names.step(players, 100, false);
    CHECK(players[1].nameTicks == 140);
    names.step(players, 200, false);
    CHECK(players[1].nameTicks == 0);
}

TEST_CASE("names are written in the initials font over the standing only",
          "[game][screens][names][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("FONTS/initials.fnt").parent_path().parent_path();
    test::assetOrSkip("STATIC/textures.ngc");
    test::FakeRenderDevice device;
    TextureSet statics;
    REQUIRE(statics.load(root / "STATIC"));
    PartyNames names;
    REQUIRE(names.load(device, root, statics));
    std::array<PlayerRuntime, 2> players;
    CharacterSave save;
    save.name = "AB";
    players[0].actor.spawn(0, save, nullptr, Vec3{0.0f}, 0.0f);
    players[1].actor.spawn(1, save, nullptr, Vec3{0.0f}, 0.0f);
    players[1].life = PlayerLife::InTower;
    PartyNames::show(players);
    names.step(players, 2, false);
    Canvas canvas;
    const Mat4 clip = glm::scale(Mat4{1.0f}, Vec3{0.01f});
    canvas.begin(device, Mat4{1.0f});
    names.draw(canvas, players, clip, 512.0f, 384.0f);
    canvas.end();
    const usize one = device.draws.size();
    CHECK(one > 0);
    // Held, nothing is written.
    device.draws.clear();
    names.step(players, 2, true);
    canvas.begin(device, Mat4{1.0f});
    names.draw(canvas, players, clip, 512.0f, 384.0f);
    canvas.end();
    CHECK(device.draws.empty());
    names.clear();
    statics.releaseTextures();
}

TEST_CASE("entry cameras hide party nametags without spending their display time",
          "[game][screens][names][assets]") {
    // do_players (0x80076998) bypasses the state/name branch while 0x803447B8 is set;
    // the entry-camera hold/ride owns that flag, separately from trigger-camera cuts.
    const auto* const stage = GENERATE("L1", "E1");
    const bool entryMarker = GENERATE(true, false);
    CAPTURE(stage, entryMarker);
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::assetOrSkip("FONTS/initials.fnt");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    TextureSet statics;
    REQUIRE(statics.load(root / "STATIC"));
    const auto initials = statics.find("INITIALS");
    REQUIRE(initials);
    const auto& image = statics.image(*initials);
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    CharacterSave first;
    first.name = "AB";
    CharacterSave second;
    second.name = "CD";
    second.color = 1;
    const std::array party{PartyMember{0, first}, PartyMember{1, second}};
    PlayOptions options;
    options.welcome = false;
    if (!entryMarker) {
        REQUIRE(world.startPoint(0));
        options.position = world.startPoint(0)->position;
    }
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const auto nameVertices = [&] {
        device.draws.clear();
        scene.render(device, makeLetterboxProjection(640, 448, 640, 448), 640, 448);
        usize count = 0;
        for (const auto& draw : device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            // HUD names share the font but are player-colored, not white overhead text.
            if (!draw.vertices.empty() && draw.vertices.front().color == Color::white() &&
                texture != nullptr && texture->pixels == image.pixels) {
                count += draw.vertices.size();
            }
        }
        return count;
    };
    REQUIRE(scene.spawning());
    CHECK(scene.startCamera().active() == entryMarker);
    CHECK(nameVertices() == 0); // includes rendering before the very first simulation tick
    bool sawRide = false;
    for (s32 ticks = 0; ticks < 1000 && scene.spawning(); ++ticks) {
        scene.update(1.0 / 60, {});
        REQUIRE(scene.runtime(0));
        REQUIRE(scene.runtime(1));
        REQUIRE(scene.runtime(0)->nameTicks == PartyNames::kTicks);
        REQUIRE(scene.runtime(1)->nameTicks == PartyNames::kTicks);
        if (ticks == 0 || (!sawRide && scene.startCamera().phase() == StartCamera::Phase::Ride)) {
            CHECK(nameVertices() == 0);
        }
        sawRide |= scene.startCamera().phase() == StartCamera::Phase::Ride;
    }
    REQUIRE_FALSE(scene.spawning());
    CHECK(sawRide == entryMarker);
    scene.update(1.0 / 60, {});
    CHECK(scene.runtime(0)->nameTicks == PartyNames::kTicks - 1);
    CHECK(scene.runtime(1)->nameTicks == PartyNames::kTicks - 1);
    CHECK(nameVertices() == 24); // two letters, six triangle vertices each, for both players
    for (s32 ticks = 0; ticks < PartyNames::kTicks - 2; ++ticks) {
        scene.update(1.0 / 60, {});
    }
    CHECK(scene.runtime(0)->nameTicks == 1);
    CHECK(nameVertices() == 24);
    scene.update(1.0 / 60, {});
    CHECK(scene.runtime(0)->nameTicks == 0);
    CHECK(nameVertices() == 0);
}

} // namespace
