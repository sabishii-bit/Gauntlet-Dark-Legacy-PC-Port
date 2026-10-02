#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/Rotators.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("turntable pads use the authored contact height and the player's half height",
          "[rotators][platform-contact]") {
    const f32 height = GENERATE(3.5f, 7.5f, 7.6f);
    const auto dir = test::scratchDirectory("rotator-contact");
    // One turntable: .01 radians per tick, stopping at one radian.
    writeTextFile(dir / "world.json", R"({
      "objects":[{"name":"TABLE","position":[0,0,0],"flags":4096}],
      "itemInfos":[{"type":12,"subtype":2,"name":"BRIDGEPAD","radius":3,"height":5}],
      "itemInstances":[{"info":0,"position":[0,0,0],
        "params":[0,0,0,0,10,215,35,60,0,0,128,63]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    Rotators rotators;
    rotators.bind(layout);
    REQUIRE(rotators.size() == 1);
    WorldScene scene;
    const std::array visitors{TriggerVisitor{.position = Vec3{0, height, 0}}};
    rotators.update(kStep, visitors, scene);
    CHECK(rotators.rotator(0).started == (height <= 7.5f));
}

TEST_CASE("the mines' gears spin for ever and a bridge pad turns its turntable into place once",
          "[game][world][rotators][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELI2/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELI2"));
    ModelSet models;
    TextureSet textures;
    REQUIRE(models.load(root / "LEVELS/LEVELI2"));
    REQUIRE(textures.load(root / "LEVELS/LEVELI2"));
    WorldScene scene;
    REQUIRE(scene.build(layout, models, textures, device));
    Rotators rotators;
    rotators.bind(layout);
    REQUIRE(rotators.size() == 5);
    const auto pad =
        std::ranges::find_if(std::views::iota(usize{0}, rotators.size()), [&](usize i) {
            return rotators.rotator(i).subtype == Rotators::kTurnedByPad;
        });
    REQUIRE(*pad < rotators.size());
    const Rotators::Rotator& table = rotators.rotator(*pad);
    CHECK(layout.objects()[static_cast<usize>(table.object)].name == "I2ELEV43");
    CHECK(table.limit == Approx(3.927f).margin(0.001f));
    CHECK(table.speed == Approx(0.0087f).margin(0.0001f));
    // With nobody on the pad, the gears turn and the turntable waits.
    const std::array<TriggerVisitor, 1> nobody{TriggerVisitor{.position = Vec3{1000, 0, 0}}};
    for (s32 i = 0; i < 30; ++i) {
        CHECK(rotators.update(kStep, nobody, scene).empty());
    }
    for (usize i = 0; i < rotators.size(); ++i) {
        const auto& rotator = rotators.rotator(i);
        if (rotator.subtype == Rotators::kSpinning) {
            CHECK(std::abs(rotator.turned) == Approx(std::abs(rotator.speed) * 60.0f));
        }
    }
    CHECK(table.turned == 0.0f);
    // Stepped on, it turns until it has gone its angle, sounding as it goes, and stops once.
    const std::array<TriggerVisitor, 1> onPad{TriggerVisitor{.position = table.spot}};
    s32 turning = 0;
    s32 stopped = 0;
    for (s32 i = 0; i < 400 && !table.done; ++i) {
        for (const RotatorCue& cue : rotators.update(kStep, i == 0 ? onPad : nobody, scene)) {
            turning += cue.kind == RotatorCue::Kind::Turning ? 1 : 0;
            stopped += cue.kind == RotatorCue::Kind::Stopped ? 1 : 0;
        }
    }
    REQUIRE(table.done);
    CHECK(stopped == 1);
    CHECK(turning > 100);
    CHECK(table.turned >= table.limit);
    CHECK(table.turned < table.limit + 0.02f);
    // Turned the way YawMat3 turns: its x axis swings to (cos, 0, -sin).
    const Mat4& placed = scene.worldTransform(static_cast<usize>(table.object));
    CHECK(placed[0].x == Approx(std::cos(table.turned)).margin(0.001f));
    CHECK(placed[0].z == Approx(-std::sin(table.turned)).margin(0.001f));
    // Done, it stays where it is.
    const f32 finalAngle = table.turned;
    CHECK(rotators.update(kStep, onPad, scene).empty());
    CHECK(table.turned == finalAngle);
}

} // namespace
