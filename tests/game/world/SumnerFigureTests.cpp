#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/SumnerFigure.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("Sumner samples between ticks without advancing gestures or blending across cuts",
          "[game][world][sumner-interpolation]") {
    const auto dir = test::scratchDirectory("sumner-interpolation");
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"GWIZ","nodes":[
        {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":4,"frameRate":30,"repeats":true,
        "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]},
        {"name":"WELCOME","frames":4,"frameRate":30,"repeats":false,
        "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[10,13]}]}]}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
        "locators":[{"type":"event","delay":0,"position":[0,0,0],"rotation":[0,0,0]}]})");
    WorldLayout layout;
    ItemArchive items;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(dir));
    REQUIRE(items.load(dir));
    SumnerFigure sumner;
    REQUIRE(sumner.load(device, items, layout));
    const auto heightAt = [&](f32 blend) {
        device.draws.clear();
        sumner.draw(device, Mat4{1}, {}, blend);
        REQUIRE(device.draws.size() == 1);
        REQUIRE(device.draws.front().vertices.size() == 3);
        return device.draws.front().vertices.front().position.y;
    };

    sumner.capturePresentation();
    sumner.update(kStep * 0.5f);
    CHECK(heightAt(0) == Approx(0));
    CHECK(heightAt(0.5f) == Approx(0.25f));
    CHECK(heightAt(1) == Approx(0.5f));
    CHECK(heightAt(0.5f) == Approx(0.25f)); // drawing never consumes the interval
    CHECK(sumner.sequence() == 0);
    CHECK(sumner.index() == 0);
    // A held simulation update cannot replay the previous interpolation interval.
    sumner.capturePresentation();
    CHECK(heightAt(0) == Approx(0.5f));
    CHECK(heightAt(1) == Approx(0.5f));
    const f32 paused = heightAt(-1);
    CHECK(heightAt(-1) == paused); // options select the fixed simulation pose

    sumner.update(kStep * 2.5f);
    CHECK(heightAt(1) == Approx(3));
    sumner.update(kStep);
    CHECK(heightAt(0) == Approx(0)); // loop generation never blends end back to start
    CHECK(heightAt(0.5f) == Approx(0));
    sumner.play(SumnerFigure::kWelcomeIndex);
    sumner.update(kStep);
    CHECK(sumner.playing(SumnerFigure::kWelcomeIndex));
    CHECK(heightAt(0) == Approx(10)); // gesture cuts do not smear between sequences
    CHECK(heightAt(0.5f) == Approx(10));
    sumner.update(kStep * 0.5f);
    CHECK(heightAt(0.5f) == Approx(10.25f));
    sumner.update(kStep * 4);
    CHECK(sumner.sequence() == 0);
    CHECK(heightAt(0.5f) == Approx(0));
    sumner.clear();
    device.draws.clear();
    sumner.draw(device, Mat4{1}, {}, 0.5f);
    CHECK(device.draws.empty());
    REQUIRE(sumner.load(device, items, layout));
    CHECK(heightAt(0.5f) == Approx(0));
}

TEST_CASE("Sumner stands at his lookout, cycles his idles and gestures on request",
          "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("ITEMS/LEVELL/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELL"));
    SumnerFigure sumner;
    REQUIRE_FALSE(sumner.loaded());
    REQUIRE(sumner.load(device, items, layout));
    REQUIRE(sumner.loaded());
    REQUIRE(sumner.position().x == Approx(2.97f).margin(0.01f));
    REQUIRE(sumner.position().z == Approx(-53.47f).margin(0.01f));
    REQUIRE(sumner.yaw() == Approx(-3.0954f + kPi).margin(0.001f)); // turned to the party
    REQUIRE(sumner.sequence() == 0);                                // the stance
    REQUIRE(sumner.index() == 0);
    REQUIRE_FALSE(sumner.gesturing());

    // The stance loops once (ninety frames), and as it wraps the cycle asks for the reading,
    // which starts when the stance next ends.
    for (s32 i = 0; i < 90; ++i) {
        sumner.update(kStep);
    }
    REQUIRE(sumner.index() == 1);
    REQUIRE(sumner.sequence() == 0);
    for (s32 i = 0; i < 90; ++i) {
        sumner.update(kStep);
    }
    REQUIRE(sumner.sequence() == 1);
    REQUIRE(sumner.index() == 2);

    sumner.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.size() > 10);

    // The welcome gesture cuts in at once and the cycle resumes from the stance after it.
    sumner.gesture();
    sumner.update(kStep);
    REQUIRE(sumner.gesturing());
    REQUIRE(sumner.sequence() == 6);
    REQUIRE(sumner.index() == 0);
    s32 steps = 0;
    while (sumner.gesturing() && steps < 130) {
        sumner.update(kStep);
        ++steps;
    }
    REQUIRE_FALSE(sumner.gesturing());
    REQUIRE(sumner.sequence() == 0);
    REQUIRE(steps > 100); // seventy-five frames at twenty a second
    // His greeting and his send-off cut in the same way, by the original's indices.
    sumner.play(SumnerFigure::kWelcomeIndex);
    sumner.update(kStep);
    REQUIRE(sumner.playing(SumnerFigure::kWelcomeIndex));
    REQUIRE(sumner.sequence() == 3);
    sumner.play(SumnerFigure::kGoAwayIndex);
    sumner.update(kStep);
    REQUIRE(sumner.playing(SumnerFigure::kGoAwayIndex));
    REQUIRE_FALSE(sumner.playing(SumnerFigure::kWelcomeIndex));
    REQUIRE_FALSE(sumner.playing(42));

    sumner.clear();
    REQUIRE_FALSE(sumner.loaded());
}

TEST_CASE("Sumner is absent without his item set", "[game][world]") {
    test::FakeRenderDevice device;
    const WorldLayout layout;
    ItemArchive items;
    REQUIRE_FALSE(items.load(test::scratchDirectory("sumner-none")));
    REQUIRE_FALSE(items.loaded());
    SumnerFigure sumner;
    REQUIRE_FALSE(sumner.load(device, items, layout));
    REQUIRE_FALSE(sumner.loaded());
    sumner.gesture();
    sumner.update(kStep);
    sumner.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty());
}

} // namespace
