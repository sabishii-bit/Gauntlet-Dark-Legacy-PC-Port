#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/SkorneArena.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Skorne masonry waits for cues, lifts twice, scatters and retires", "[skorne]") {
    const auto directory = test::scratchDirectory("skorne-arena");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "bounds":{"min":[-20,-10,-20],"max":[20,20,20]},
      "itemInfos":[{"type":10,"subtype":51}],
      "itemInstances":[{"info":0,"name":"ROCK","position":[10,0,0]}]})");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"ROCK","file":"rock.obj","meshTriangles":1}]})");
    writeTextFile(directory / "rock.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl tex0\nf 1 2 3\n");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    SkorneArena arena;
    arena.bind(device, layout, models, textures);
    REQUIRE(arena.size() == 1);
    arena.update(2);
    REQUIRE(arena.position(0) == Vec3{10, 0, 0});
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    const f32 first = arena.position(0).y;
    REQUIRE(first >= 18.0f / 30);
    REQUIRE(first < 28.0f / 30);
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    REQUIRE(arena.position(0).y >= first + 28.0f / 30);
    REQUIRE(arena.position(0).x == 10);
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    REQUIRE(arena.position(0).x > 10);
    REQUIRE(arena.phase() == 3);
    arena.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    arena.update(20);
    REQUIRE_FALSE(arena.visible(0));
    arena.cue(Vec3{0});
    REQUIRE_FALSE(arena.visible(0));
    device.draws.clear();
    arena.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.empty());
    arena.clear();
    REQUIRE(arena.size() == 0);
    REQUIRE(arena.phase() == 0);
}

TEST_CASE("altar entrance loads all twenty-two loose masonry meshes", "[skorne][unpacked]") {
    const auto directory = test::unpackedOrSkip("LEVELS/LEVELE2/world.json").parent_path();
    test::unpackedOrSkip("LEVELS/LEVELE2/objects.json");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    SkorneArena arena;
    arena.bind(device, layout, models, textures);
    REQUIRE(arena.size() == 22);
    arena.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() >= 22);
}
} // namespace
