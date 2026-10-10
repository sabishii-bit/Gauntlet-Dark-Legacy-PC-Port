#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/CritterStatues.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr f32 kPi = std::numbers::pi_v<f32>;

/** An archive holding a golem's statue: idle, then a ten-frame ACTIVE at thirty a second. */
std::filesystem::path statueArchive() {
    const auto root = test::scratchDirectory("critter-statues");
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    writeTextFile(root / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
      {"index":0,"name":"STONE","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(root / "textures/skin.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(root);
    writeTextFile(root / "animations.json", R"({"trees":[{"name":"GOL_STATUE",
      "nodes":[{"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"IDLE","frames":0,"frameRate":30},
                   {"name":"ACTIVE","frames":10,"frameRate":30}]}]})");
    return root;
}

CritterStatues::Placement golemAt(const Vec3& position, f32 sight = 10.0f) {
    CritterStatues::Placement placement;
    placement.kind = CombatantKind::Golem;
    placement.instance.position = position;
    placement.radius = 4.0f;
    placement.height = 5.0f;
    placement.viewRadius = 10.0f;
    placement.sight = sight;
    placement.carried = 7;
    return placement;
}

TEST_CASE("a golem statue wakes on approach before body contact, blocks walking, takes a blow, "
          "and rises as its ACTIVE sequence runs its ticks in view",
          "[game][enemies][critter-statues]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(statueArchive()));
    CritterStatues statues;
    REQUIRE(CritterStatues::treeOf(CombatantKind::Golem) == "GOL_STATUE");
    REQUIRE(CritterStatues::treeOf(CombatantKind::Gargoyle) == "GAR_STATUE");
    REQUIRE(CritterStatues::treeOf(CombatantKind::General).empty());
    // Only the kinds that stand as statues, and only from an archive holding the tree.
    CritterStatues::Placement general = golemAt(Vec3{0});
    general.kind = CombatantKind::General;
    REQUIRE_FALSE(statues.add(device, archive, general, nullptr));
    CritterStatues::Placement gargoyle = golemAt(Vec3{0});
    gargoyle.kind = CombatantKind::Gargoyle;
    REQUIRE_FALSE(statues.add(device, archive, gargoyle, nullptr));
    REQUIRE(statues.add(device, archive, golemAt(Vec3{0, 0, 0}), nullptr));
    REQUIRE(statues.add(device, archive, golemAt(Vec3{30, 0, 0}, -1.0f), nullptr));
    REQUIRE(statues.count() == 2);
    REQUIRE(statues.placement(0).carried == 7);
    REQUIRE_FALSE(statues.woken(0));
    // Each puts an upright cylinder of its record's radius in the way, and offers it to blows.
    const auto cylinders = statues.obstacles();
    REQUIRE(cylinders.size() == 2);
    REQUIRE(cylinders[0].cylinderRadius == 4.0f);
    REQUIRE(cylinders[0].height == 5.0f);
    REQUIRE(cylinders[1].centre.x == 30.0f);
    const auto targets = statues.targets();
    REQUIRE(targets.size() == 2);
    REQUIRE(targets[1].id == 1);
    REQUIRE(targets[1].radius == 4.0f);
    // Sight wakes the statue while the player is still clear of its solid cylinder.
    // Contact separately pushes the player out; negative sight disables approach waking.
    REQUIRE(statues.touch(Vec3{0, 0, 12}, 1.0f) == Vec3{0, 0, 12});
    REQUIRE_FALSE(statues.woken(0));
    REQUIRE(statues.touch(Vec3{0, 0, 10}, 1.0f) == Vec3{0, 0, 10});
    REQUIRE(statues.woken(0));
    REQUIRE(statues.touch(Vec3{0, 0, 4.5f}, 1.0f) == Vec3{0, 0, 5});
    REQUIRE(statues.woken(0));
    REQUIRE(statues.touch(Vec3{30, 0, 4.5f}, 1.0f) == Vec3{30, 0, 5});
    REQUIRE_FALSE(statues.woken(1));
    // The nearest still asleep, for a trigger to wake.
    const auto asleep = statues.nearestAsleep(Vec3{20, 0, 0});
    REQUIRE(asleep.has_value());
    REQUIRE(asleep->first == 1);
    REQUIRE(asleep->second == 10.0f);
    // Out of view, a woken statue waits; in view it starts its ACTIVE sequence, and after
    // that sequence's ticks (ten frames at thirty a second: twenty) it has risen.
    statues.update(kTicks, kStep, [](const Vec3&, f32) { return false; });
    REQUIRE_FALSE(statues.rising(0));
    std::vector<Vec3> looked;
    const auto seen = [&](const Vec3& at, f32 radius) {
        looked.push_back(at);
        return radius == 10.0f;
    };
    statues.update(kTicks, kStep, seen);
    REQUIRE(looked.size() == 1); // only the woken one is looked for
    REQUIRE(statues.rising(0));
    REQUIRE(statues.takeRisen().empty());
    for (s32 i = 0; i < 9; ++i) {
        statues.update(kTicks, kStep, seen);
    }
    REQUIRE(statues.count() == 2);
    statues.update(kTicks, kStep, seen);
    REQUIRE(statues.count() == 1);
    auto risen = statues.takeRisen();
    REQUIRE(risen.size() == 1);
    REQUIRE(risen[0].kind == CombatantKind::Golem);
    REQUIRE(risen[0].carried == 7);
    REQUIRE(risen[0].instance.position == Vec3{0, 0, 0});
    REQUIRE(statues.takeRisen().empty());
    // A blow wakes the other; a record's activeOn (in half ticks) sets the wait instead.
    REQUIRE(statues.add(device, archive, golemAt(Vec3{60, 0, 0}), nullptr));
    CritterStatues::Placement quick = golemAt(Vec3{90, 0, 0});
    quick.activeOn = 3;
    REQUIRE(statues.add(device, archive, quick, nullptr));
    statues.wake(0);
    statues.wake(2);
    REQUIRE(statues.woken(0));
    REQUIRE_FALSE(statues.woken(1));
    REQUIRE(statues.woken(2));
    statues.update(kTicks, kStep, nullptr); // no view: everything is in view
    REQUIRE(statues.rising(0));
    REQUIRE(statues.rising(2));
    for (s32 i = 0; i < 3; ++i) {
        statues.update(kTicks, kStep, nullptr);
    }
    risen = statues.takeRisen();
    REQUIRE(risen.size() == 1);
    REQUIRE(risen[0].instance.position == Vec3{90, 0, 0});
    REQUIRE(statues.count() == 2);
    statues.clear();
    REQUIRE(statues.count() == 0);
}

TEST_CASE("a statue with sight smaller than its body can be approached into wake range",
          "[game][enemies][critter-statues]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(statueArchive()));
    CritterStatues statues;
    REQUIRE(statues.add(device, archive, golemAt(Vec3{0}, 2), nullptr));
    CHECK(statues.touch(Vec3{0, 0, 4}, 1) == Vec3{0, 0, 4});
    CHECK_FALSE(statues.woken(0));
    CHECK(statues.touch(Vec3{0, 8, 2}, 1, 4) == Vec3{0, 8, 2});
    CHECK_FALSE(statues.woken(0));
    CHECK(statues.touch(Vec3{0, 0, 2.5f}, 1) == Vec3{0, 0, 5});
    CHECK(statues.woken(0));
}

TEST_CASE("a gargoyle wakes on approach and plays its eighty-five-frame ACTIVE sequence",
          "[game][enemies][critter-statues][assets]") {
    const auto root = test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    CritterStatues statues;
    CritterStatues::Placement placement;
    placement.kind = CombatantKind::Gargoyle;
    placement.form = "GAR_EAGL";
    placement.instance.position = Vec3{-75.0f, 0.1f, 32.7f};
    placement.instance.rotation = Vec3{kPi, 0.2246915f, -kPi};
    placement.radius = 8.0f;
    placement.height = 5.0f;
    placement.viewRadius = 16.0f;
    placement.sight = 30.0f;
    REQUIRE(statues.add(device, archive, placement, nullptr));
    const Vec3 approached = placement.instance.position + Vec3{0, 0, 20};
    REQUIRE(statues.touch(approached, 1) == approached);
    REQUIRE(statues.woken(0));
    statues.update(kTicks, kStep, nullptr);
    REQUIRE(statues.rising(0));
    // Eighty-five frames at thirty a second: 170 ticks, the frame's two at a time.
    for (s32 i = 0; i < 84; ++i) {
        statues.update(kTicks, kStep, nullptr);
    }
    REQUIRE(statues.count() == 1);
    statues.update(kTicks, kStep, nullptr);
    REQUIRE(statues.count() == 0);
    const auto risen = statues.takeRisen();
    REQUIRE(risen.size() == 1);
    REQUIRE(risen[0].form == "GAR_EAGL");
}

TEST_CASE("statue sight is bounded by the native item query cells without becoming a fixed cap",
          "[game][enemies][critter-statues][statue-query]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(statueArchive()));
    CritterStatues statues;
    // InitDynGrid: max X/Z extent / 64 = ten-unit cells, with a nonzero origin.
    statues.setContactGridBounds(Vec3{-320, -20, -320}, Vec3{320, 20, 320});
    REQUIRE(statues.add(device, archive, golemAt(Vec3{5, 0, 5}, 30), nullptr));
    // Player cell 35 queries down to cell 33: the statue in cell 32 is not visited,
    // although it is inside the authored sight circle (distance 30, reach 31).
    CHECK(statues.touch(Vec3{35, 0, 5}, 1) == Vec3{35, 0, 5});
    CHECK_FALSE(statues.woken(0));
    // Whole-cell rounding permits this 20-unit separation. A fixed 16-unit cap
    // would incorrectly delay the wake, as would rounding instead of truncation.
    CHECK(statues.touch(Vec3{25, 0, 5}, 1) == Vec3{25, 0, 5});
    CHECK(statues.woken(0));

    REQUIRE(statues.add(device, archive, golemAt(Vec3{5, 0, 5}, 30), nullptr));
    CHECK(statues.touch(Vec3{5, 0, 35}, 1) == Vec3{5, 0, 35});
    CHECK_FALSE(statues.woken(1));
    CHECK(statues.touch(Vec3{5, 0, 25}, 1) == Vec3{5, 0, 25});
    CHECK(statues.woken(1));

    // The rectangle is only the broad phase: its diagonal still needs sight.
    REQUIRE(statues.add(device, archive, golemAt(Vec3{5, 0, 5}, 10), nullptr));
    statues.touch(Vec3{15, 0, 15}, 1);
    CHECK_FALSE(statues.woken(2));
    statues.touch(Vec3{15, 0, 5}, 1);
    CHECK(statues.woken(2));

    // Trigger/weapon wake events are independent of the player's query cells.
    REQUIRE(statues.add(device, archive, golemAt(Vec3{105, 0, 105}, -1), nullptr));
    statues.wake(3);
    CHECK(statues.woken(3));
    statues.clear();
    REQUIRE(statues.add(device, archive, golemAt(Vec3{5, 0, 5}, 30), nullptr));
    statues.touch(Vec3{35, 0, 5}, 1);
    CHECK(statues.woken(0)); // a new level cannot inherit the old grid
}

} // namespace
