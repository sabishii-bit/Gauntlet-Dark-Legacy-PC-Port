#include <filesystem>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/Progression.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<PlayerFigure>);
static_assert(!std::is_copy_constructible_v<PlayerFigure>);

/** A costume with a wrist and an unmapped ornament, sharing a tiny synthetic mesh. */
std::filesystem::path costumeFixture(std::string_view name, bool animated) {
    const auto root = test::scratchDirectory(name);
    const auto costume = root / "PLAYERS/WAR/BLU";
    std::filesystem::create_directories(costume);
    writeTextFile(costume / "mesh.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(costume / "skin.png", test::kTinyPng);
    writeTextFile(costume / "objects.json", R"({"objects":[
        {"index":0,"name":"R_WRIST","file":"mesh.obj","meshTriangles":1},
        {"index":1,"name":"ORNAMENT","file":"mesh.obj","meshTriangles":1},
        {"index":2,"name":"WEAP_HOLD","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(costume / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(costume / "animations.json", R"({"trees":[
        {"name":"WAR_BLU","nodes":[
            {"name":"HAND","object":"R_WRIST","position":[1,2,3]},
            {"name":"ORNAMENT","object":"ORNAMENT","position":[4,0,0]}],"sequences":[]}]})");
    if (animated) {
        const auto actions = root / "PLAYERS/WAR/ANIM";
        std::filesystem::create_directories(actions);
        // HAND is deliberately at a different index; ORNAMENT has no class counterpart.
        writeTextFile(actions / "animations.json", R"({"trees":[
            {"name":"WAR","nodes":[
                {"name":"UNUSED","position":[0,0,0]},
                {"name":"HAND","position":[7,8,9]}],
             "sequences":[{"name":"READY","frames":60,"frameRate":30,"repeats":true}]}]})");
    }
    return root;
}

TEST_CASE("an unloaded player figure is safe to animate and draw", "[game][world][figure]") {
    test::FakeRenderDevice device;
    PlayerFigure figure;
    REQUIRE_FALSE(figure.heldWeaponBound());
    REQUIRE_FALSE(figure.animator().bound());
    REQUIRE_FALSE(figure.missile().bound());
    REQUIRE_FALSE(figure.throwSound().has_value());
    REQUIRE(figure.effects() == nullptr);
    REQUIRE_FALSE(figure.handPosition(Mat4{1.0f}).has_value());
    figure.animate(1.0f, 2, 1.0f / 30.0f);
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, false);
    REQUIRE(device.draws.empty());
    REQUIRE(PlayerFigure::load(device, test::scratchDirectory("figure-missing"), CharacterSave{}) ==
            nullptr);
}

TEST_CASE("player figures select costume tiers without requiring a scene",
          "[game][world][figure]") {
    const auto root = test::scratchDirectory("figure-costume-tiers");
    CharacterSave save;
    save.color = 1;
    save.progress().experience = levelExperience(25);
    REQUIRE(PlayerFigure::costumeDirectory(root, save).filename() == "BLU");
    std::filesystem::create_directories(root / "PLAYERS/WAR/BLU20");
    writeTextFile(root / "PLAYERS/WAR/BLU20/objects.json", "{}");
    REQUIRE(PlayerFigure::costumeDirectory(root, save).filename() == "BLU20");
}

TEST_CASE("a player figure draws a rest-pose weapon when optional animations are missing",
          "[game][world][figure]") {
    const auto root = costumeFixture("figure-rest-pose", false);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->directory() == root / "PLAYERS/WAR/BLU");
    REQUIRE(figure->heldWeaponBound());
    REQUIRE_FALSE(figure->animator().bound());
    REQUIRE_FALSE(figure->handPosition(Mat4{1.0f}).has_value());
    figure->animate(1.0f, 2, 1.0f / 30.0f);
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, false);
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws.back().vertices[0].position == Vec3{1.0f, 2.0f, 3.0f});
    device.draws.clear();
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, true);
    REQUIRE(device.draws.size() == 2);
}

TEST_CASE("a player figure maps animation nodes by name and poses its held weapon",
          "[game][world][figure]") {
    const auto root = costumeFixture("figure-posed", true);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->animator().bound());
    const Mat4 body = glm::scale(glm::translate(Mat4{1.0f}, Vec3{10.0f, 20.0f, 30.0f}), Vec3{2.0f});
    const auto hand = figure->handPosition(body);
    REQUIRE(hand.has_value());
    REQUIRE(*hand == Vec3{24.0f, 36.0f, 48.0f});
    figure->animate(0.0f, 2, 1.0f / 30.0f);
    figure->draw(device, Mat4{1.0f}, body, {}, 0.5f, false);
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws[0].vertices[0].position == *hand);
    REQUIRE(device.draws[1].vertices[0].position == Vec3{18.0f, 20.0f, 30.0f});
    REQUIRE(device.draws[2].vertices[0].position == *hand);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.state.blend == BlendMode::Alpha);
        REQUIRE_FALSE(draw.state.depthWrite);
    }
}
} // namespace
