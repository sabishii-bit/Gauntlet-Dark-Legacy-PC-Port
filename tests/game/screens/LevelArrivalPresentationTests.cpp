#include <array>
#include <filesystem>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelArrivalPresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path spawnFixture(std::string_view name, bool animated = true,
                                   bool cycle = false) {
    const auto root = test::scratchDirectory(name);
    writeTextFile(root / "mesh.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                                     "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(root / "skin.png", test::kTinyPng);
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(root / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0},
        {"index":1,"name":"FRAME0","file":"skin.png","width":2,"height":2,"flags":0},
        {"index":2,"name":"FRAME1","file":"skin.png","width":2,"height":2,"flags":0}]})");
    const std::string sequence = animated ? R"({"name":"START","frames":8,"frameRate":30,
        "repeats":false,"tracks":[{"node":0,"flags":16,"frames":[0,4],"values":[0,4]}]})"
                                          : "";
    const std::string texture = cycle ? R"({"texture":0,"source":1,"frames":2,"rate":1})"
                                      : R"({"texture":0,"source":-2,"frames":4})";
    writeTextFile(root / "animations.json", R"({"textureAnimations":[)" + texture +
                                                R"(],"trees":[{"name":"STARTFX","nodes":[
        {"name":"ROOT","object":"BODY","position":[0,0,0]}],"sequences":[)" +
                                                sequence + "]}]}");
    return root;
}

TEST_CASE("arrival without art still holds the party for the spawn interval",
          "[game][screens][arrival]") {
    test::FakeRenderDevice device;
    ItemArchive missing;
    LevelArrivalPresentation arrival;
    REQUIRE_FALSE(arrival.active());
    arrival.animate(1.0f);
    arrival.advance(5, false, Vec3{0.0f}, Vec3{0.0f});
    arrival.drawEffects(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
    const std::array<Vec3, 1> party{Vec3{1, 2, 3}};
    arrival.begin(device, missing, party);
    REQUIRE(arrival.active());
    REQUIRE_FALSE(arrival.camera().active());
    REQUIRE(arrival.effectCount() == 0);
    arrival.advance(LevelArrivalPresentation::kSpawnTicks - 1, true, Vec3{0.0f}, Vec3{0.0f});
    REQUIRE(arrival.active());
    arrival.advance(1, true, Vec3{0.0f}, Vec3{0.0f});
    REQUIRE_FALSE(arrival.active());
    arrival.begin(device, missing, {});
    REQUIRE(arrival.active());
    arrival.clear();
    arrival.clear();
    REQUIRE_FALSE(arrival.active());
}

TEST_CASE("arrival camera frames the party and outlasts the spawn effects",
          "[game][screens][arrival]") {
    ItemArchive archive;
    REQUIRE(archive.load(spawnFixture("arrival-camera")));
    test::FakeRenderDevice device;
    LevelArrivalPresentation arrival;
    const std::array<Vec3, 2> party{Vec3{-2, 0, 10}, Vec3{2, 0, 10}};
    WorldCamera marker;
    marker.position = Vec3{0, 10, 0};
    arrival.begin(device, archive, party, marker);
    REQUIRE(arrival.effectCount() == 2);
    REQUIRE(arrival.camera().phase() == StartCamera::Phase::Hold);
    REQUIRE(arrival.camera().attention().z == Approx(std::sqrt(200.0f)));
    const Vec3 follow{0, 10, -10};
    const Vec3 attention{0, 0, 10};
    // Visual animation must not advance the camera before the listener samples it.
    arrival.animate(1.0f / 30.0f);
    REQUIRE(arrival.camera().ticksLeft() == StartCamera::kHoldTicks);
    arrival.advance(LevelArrivalPresentation::kSpawnTicks, false, follow, attention);
    REQUIRE(arrival.active());
    REQUIRE(arrival.camera().phase() == StartCamera::Phase::Hold);
    arrival.drawEffects(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
    arrival.advance(1, true, follow, attention);
    REQUIRE(arrival.camera().phase() == StartCamera::Phase::Ride);
    for (s32 ticks = 0; ticks < 300 && arrival.active(); ++ticks) {
        arrival.advance(1, false, follow, attention);
    }
    REQUIRE_FALSE(arrival.active());
    REQUIRE(glm::distance(arrival.camera().camera().position, follow) < StartCamera::kArrival);
    // A new arrival without a marker cannot reuse the previous camera's hold.
    arrival.begin(device, archive, party);
    REQUIRE_FALSE(arrival.camera().active());
    REQUIRE(arrival.active());
}

TEST_CASE("arrival effects preserve placement lighting animation and fractional texture steps",
          "[game][screens][arrival]") {
    ItemArchive archive;
    REQUIRE(archive.load(spawnFixture("arrival-animation")));
    test::FakeRenderDevice device;
    LevelArrivalPresentation arrival;
    const std::array<Vec3, 2> party{Vec3{2, 3, 4}, Vec3{-2, 3, 4}};
    arrival.begin(device, archive, party);
    WorldLighting lighting;
    lighting.ambient = Vec3{0.25f, 0.5f, 0.75f};
    lighting.lightColor = Vec3{0.0f};
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].vertices[0].position == party[0]);
    REQUIRE(device.draws[1].vertices[0].position == party[1]);
    REQUIRE(device.draws[0].vertices[0].color == lighting.shade(Vec3{0, 1, 0}));
    REQUIRE(device.draws[0].state.uvOffset == Vec2{0.0f});
    device.draws.clear();
    arrival.animate(1.0f / 60.0f);
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws[0].state.uvOffset == Vec2{0.0f});
    device.draws.clear();
    arrival.animate(1.0f / 60.0f);
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws[0].vertices[0].position.x == Approx(3.0f));
    REQUIRE(device.draws[1].vertices[0].position.x == Approx(-1.0f));
    REQUIRE(device.draws[0].state.uvOffset == Vec2{0.25f, 0.0f});
    device.draws.clear();
    arrival.animate(1.0f); // animation finishes and holds, without looping
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    const Vec3 finished = device.draws[0].vertices[0].position;
    device.draws.clear();
    arrival.animate(1.0f);
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws[0].vertices[0].position == finished);
    arrival.clear();
    archive.clear();
    device.draws.clear();
    arrival.animate(1.0f);
    arrival.drawEffects(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.empty());
    REQUIRE(arrival.effectCount() == 0);
}

TEST_CASE("static arrival effects cycle textures and rebinding clears borrowed art",
          "[game][screens][arrival]") {
    ItemArchive archive;
    REQUIRE(archive.load(spawnFixture("arrival-static", false, true)));
    test::FakeRenderDevice device;
    LevelArrivalPresentation arrival;
    const std::array<Vec3, 1> party{Vec3{3, 4, 5}};
    arrival.begin(device, archive, party);
    arrival.animate(1.0f / 30.0f);
    arrival.drawEffects(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].vertices[0].position == party[0]);
    REQUIRE(device.draws[0].texture == &archive.textures.texture(device, 2));
    device.draws.clear();
    arrival.animate(1.0f / 30.0f);
    arrival.drawEffects(device, Mat4{1.0f}, {});
    REQUIRE(device.draws[0].texture == &archive.textures.texture(device, 1));
    ItemArchive missing;
    arrival.begin(device, missing, party);
    archive.clear();
    device.draws.clear();
    arrival.animate(1.0f);
    arrival.drawEffects(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
    REQUIRE(arrival.effectCount() == 0);
    REQUIRE(arrival.active());
}

TEST_CASE("arrival title slides during the hold and disappears when the arrival finishes",
          "[game][screens][arrival]") {
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}});
    const test::FakeTexture sheet{32, 16};
    TextPainter text;
    text.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    Canvas canvas;
    ItemArchive missing;
    LevelArrivalPresentation arrival;
    arrival.begin(device, missing, {}, WorldCamera{});
    const auto draw = [&](std::string_view title) {
        device.draws.clear();
        canvas.begin(device, Mat4{1.0f});
        arrival.drawTitle(canvas, text, title, 512);
        canvas.end();
    };
    draw("A");
    REQUIRE(device.draws.size() == 1);
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{253.5f, 48.5f});
    arrival.advance(20, false, Vec3{0.0f}, Vec3{0.0f});
    draw("A");
    REQUIRE(test::minCorner(device.draws[0]).y == Approx(40.5f));
    arrival.advance(60, false, Vec3{0.0f}, Vec3{0.0f});
    draw("A");
    REQUIRE(test::minCorner(device.draws[0]).y == Approx(16.5f));
    draw("");
    REQUIRE(device.draws.empty());
    const TextPainter unavailable;
    canvas.begin(device, Mat4{1.0f});
    arrival.drawTitle(canvas, unavailable, "A", 512);
    canvas.end();
    REQUIRE(device.draws.empty());
    arrival.advance(11, false, Vec3{0.0f}, Vec3{0.0f});
    REQUIRE(arrival.camera().phase() == StartCamera::Phase::Ride);
    arrival.advance(1, false, Vec3{0.0f}, Vec3{0.0f});
    REQUIRE_FALSE(arrival.active());
    draw("A");
    REQUIRE(device.draws.empty());
    arrival.begin(device, missing, {});
    arrival.advance(1, false, Vec3{0.0f}, Vec3{0.0f});
    draw("A");
    REQUIRE(test::minCorner(device.draws[0]).y == Approx(16.5f)); // no ride: title sits
    arrival.clear();
    draw("A");
    REQUIRE(device.draws.empty());
}
} // namespace
