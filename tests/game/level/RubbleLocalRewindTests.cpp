#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/NativeStateResources.h"
#include "game/level/Rubble.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `rubble` now holds exactly the values `saved` holds. */
bool holds(const Rubble& rubble, const Rubble::LocalSnapshot& saved) {
    Rubble::LocalSnapshot now;
    rubble.saveLocal(now);
    return rubble.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive empty, items;
    std::array<ItemArchive*, 2> archives{&empty, &items};
    Rubble rubble;
    Fixture() {
        const auto root = test::scratchDirectory("rubble-checkpoint");
        writeTextFile(root / "tri.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(root / "objects.json", R"({"objects":[
          {"index":0,"name":"BAREXP0","file":"tri.obj","meshTriangles":1},
          {"index":1,"name":"BARPOI0","file":"tri.obj","meshTriangles":1},
          {"index":2,"name":"UNKNOWN","file":"tri.obj","meshTriangles":1}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[{"name":"BARREL",
          "nodes":[{"name":"ROOT","object":"BAREXP0","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":1,"frameRate":30}]}]})");
        test::convertModelFixture(root);
        REQUIRE(items.load(root));
        // Binds the native debris meshes once beforehand, so later births upload nothing.
        Rubble preload;
        REQUIRE((preload.leave(device, archives, Rubble::kBlownBarrel, Mat4{1}) &&
                 preload.leave(device, archives, Rubble::kGasBarrel, Mat4{1})));
    }
    /** The debris's native digest over its own archive, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        REQUIRE(resources.registerArchive(items, "stage/items/0"));
        return test::liveDigest(rubble, resources);
    }
};

TEST_CASE("debris births match across independently bound owners and rewind without uploads",
          "[rollback][rubble-local-rewind]") {
    Fixture first;
    Fixture second;
    Rubble::LocalSnapshot empty;
    first.rubble.saveLocal(empty);
    const auto where = glm::translate(Mat4{1}, Vec3{4, 2, 8});
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kBlownBarrel, where));
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    Rubble::LocalSnapshot state;
    first.rubble.saveLocal(state);
    const auto textures = second.device.texturesCreated;
    const auto uploads = second.device.textureUpdates;
    REQUIRE(second.rubble.leave(second.device, second.archives, Rubble::kBlownBarrel, where));
    REQUIRE(second.rubble.leave(second.device, second.archives, Rubble::kGasBarrel, Mat4{1}));
    CHECK(second.digest() == first.digest());
    CHECK(second.rubble.transform(0) == where);
    second.rubble.draw(second.device, Mat4{1}, {});
    REQUIRE(second.device.draws.size() == 2);
    CHECK(second.device.draws[0].vertices[0].position.x >= 4);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == uploads);
    REQUIRE(first.rubble.restoreLocal(empty));
    CHECK(holds(first.rubble, empty));
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kBlownBarrel, where));
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    CHECK(holds(first.rubble, state));
}

TEST_CASE("native debris history removes predicted births and reuses bound models during replay",
          "[local-rewind][rubble-local-rewind]") {
    Fixture reference;
    Fixture corrected;
    LocalStateHistory<Rubble::LocalSnapshot> history;
    std::array<Rubble::LocalSnapshot, 8> expectedLocal;
    Rubble::LocalSnapshot observed;
    const auto textures = corrected.device.texturesCreated;
    const auto updates = corrected.device.textureUpdates;
    auto spawn = [](Fixture& fixture, u64 tick) {
        if (tick % 3 != 0) {
            return;
        }
        const auto object = tick % 2 == 0 ? Rubble::kBlownBarrel : Rubble::kGasBarrel;
        const auto placement = glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 2, 8});
        REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, object, placement));
    };
    u64 tick = 0;
    for (u64 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.rubble.saveLocal(saved);
            return true;
        }));
        const u64 distance = 1 + window % 8;
        const auto* saved = history.find(tick);
        REQUIRE(saved);
        for (u64 offset = 0; offset < distance; ++offset) {
            spawn(corrected, tick + offset);
            corrected.rubble.saveLocal(expectedLocal[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.rubble.restoreLocal(*saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            spawn(reference, tick + offset);
            REQUIRE(corrected.rubble.leave(corrected.device, corrected.archives, "UNKNOWN",
                                           glm::translate(Mat4{1}, Vec3{-40, 10, -20})));
        }
        // Native history has no catalogue admission step, even for a first-time birth.
        REQUIRE(corrected.rubble.restoreLocal(*saved));
        CHECK(holds(corrected.rubble, *saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            spawn(corrected, tick + offset);
            corrected.rubble.saveLocal(observed);
            CHECK(corrected.rubble.equalLocal(observed, expectedLocal[static_cast<usize>(offset)]));
        }
        CHECK(corrected.digest() == reference.digest());
        REQUIRE(corrected.rubble.size() == reference.rubble.size());
        for (usize index = 0; index < reference.rubble.size(); ++index) {
            CHECK(corrected.rubble.transform(index) == reference.rubble.transform(index));
        }
        reference.device.draws.clear();
        corrected.device.draws.clear();
        reference.rubble.draw(reference.device, Mat4{1}, {});
        corrected.rubble.draw(corrected.device, Mat4{1}, {});
        REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
        for (usize index = 0; index < reference.device.draws.size(); ++index) {
            CHECK(corrected.device.draws[index].vertices == reference.device.draws[index].vertices);
            CHECK(corrected.device.draws[index].transform ==
                  reference.device.draws[index].transform);
            CHECK(corrected.device.draws[index].state == reference.device.draws[index].state);
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == textures);
    CHECK(corrected.device.textureUpdates == updates);
}

TEST_CASE("native debris snapshots retain uncatalogued values but reject replaced owners",
          "[local-rewind][rubble-local-rewind]") {
    Fixture first;
    Fixture second;
    Rubble::LocalSnapshot saved;
    CHECK_FALSE(first.rubble.restoreLocal(saved));
    CHECK_FALSE(first.rubble.equalLocal(saved, saved));
    REQUIRE(first.rubble.leave(first.device, first.archives, "UNKNOWN", Mat4{1}));
    first.rubble.saveLocal(saved);
    CHECK_FALSE(second.rubble.restoreLocal(saved));
    CHECK_FALSE(second.rubble.equalLocal(saved, saved));
    CHECK(second.rubble.size() == 0);
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    REQUIRE(first.rubble.restoreLocal(saved));
    REQUIRE(first.rubble.size() == 1);
    CHECK(first.rubble.presentation()[0].object == "UNKNOWN");
    first.rubble.clear();
    CHECK_FALSE(first.rubble.restoreLocal(saved));
    CHECK_FALSE(first.rubble.equalLocal(saved, saved));
    REQUIRE(first.rubble.leave(first.device, first.archives, "UNKNOWN", Mat4{1}));
    CHECK_FALSE(first.rubble.restoreLocal(saved));
}

TEST_CASE("native debris equality distinguishes same-count mesh identities and placement",
          "[local-rewind][rubble-local-rewind][native-equality]") {
    Fixture fixture;
    Rubble::LocalSnapshot empty;
    Rubble::LocalSnapshot barrel;
    Rubble::LocalSnapshot gas;
    Rubble::LocalSnapshot moved;
    Rubble::LocalSnapshot replay;
    fixture.rubble.saveLocal(empty);
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel, Mat4{1}));
    fixture.rubble.saveLocal(barrel);
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kGasBarrel, Mat4{1}));
    fixture.rubble.saveLocal(gas);
    CHECK_FALSE(fixture.rubble.equalLocal(barrel, gas));
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel,
                                 glm::translate(Mat4{1}, Vec3{0, 1, 0})));
    fixture.rubble.saveLocal(moved);
    CHECK_FALSE(fixture.rubble.equalLocal(barrel, moved));
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel, Mat4{1}));
    fixture.rubble.saveLocal(replay);
    CHECK(fixture.rubble.equalLocal(barrel, replay)); // retained immutable model cache
}
} // namespace
