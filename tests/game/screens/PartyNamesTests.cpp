#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PartyNames.h"

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

} // namespace
