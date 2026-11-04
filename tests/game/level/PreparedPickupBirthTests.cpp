#include <array>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/NativeStateResources.h"
#include "game/level/PlacedItems.h"
#include "game/level/StageRegistry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `items` now hold exactly the values `saved` holds. */
bool holds(const PlacedItems& items, const PlacedItems::LocalSnapshot& saved) {
    PlacedItems::LocalSnapshot now;
    items.saveLocal(now);
    return items.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive archive;
    WorldCollision collision;
    PlacedItems items;
    s32 support = 0;

    explicit Fixture(std::string_view tag, s32 floor = 1) : support(floor) {
        const auto directory = test::scratchDirectory(tag);
        writeTextFile(directory / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(directory / "objects.json",
                      R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeFile(directory / "white.png", test::kTinyPng);
        writeTextFile(directory / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"FLAME0","file":"white.png","width":2,"height":2},
          {"name":"FLAME1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(directory / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"FLAME0",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"FLAME","texture":1,"source":1,"frames":2,"rate":1}],
          "trees":[
            {"name":"AAA","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
            {"name":"COIN","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"EMPTY","parent":0,"position":[0,1,0]},
              {"name":"FIRE","particle":0,"parent":1,"position":[0,1,0]},
              {"name":"TRAIL","particle":0,"parent":0,"position":[1,0,0]}],
             "sequences":[{"name":"IDLE","frames":4,"frameRate":30,
               "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]}]})");
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"ROOT","position":[0,0,0]}],
          "itemInfos":[
            {"type":1,"subtype":1,"name":"COIN","value":50,"radius":1,"height":2},
            {"type":1,"subtype":1,"name":"COIN","value":7,"radius":2,"height":3},
            {"type":1,"subtype":1,"name":"AAA","value":11,"radius":1,"height":2},
            {"type":1,"subtype":1,"name":"MISSING","value":99,"radius":1,"height":2},
            {"type":4,"subtype":1,"name":"DEATH","radius":1,"height":2}],
          "itemInstances":[{"info":0,"position":[40,0,0]}]})");
        test::convertModelFixture(directory);
        REQUIRE(layout.load(directory));
        REQUIRE(archive.load(directory));
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}}, 0, 4}});
        if (support == 2) {
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(items.bind(device, layout, world(), std::array{&archive}));
        items.setPlayerCount(2);
    }

    const WorldCollision* world() const { return support == 0 ? nullptr : &collision; }
    /** The pickups' native digest over their own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        test::registerItemStage(resources, archive, layout, world(), device);
        return test::liveDigest(items, resources);
    }
    /** Where the pickup a birth appended lies, or nothing when the birth failed. */
    std::optional<usize> appended(bool born) const {
        return born ? std::optional{items.size() - 1} : std::nullopt;
    }
    /** The clock of the archive's texture animations. */
    TextureAnimator::Clock clock() const {
        const auto* animator = items.textureAnimator(&archive);
        REQUIRE(animator != nullptr);
        return animator->clock();
    }
    void step(f32 seconds) {
        items.capturePresentation();
        items.update(seconds);
    }
};

void checkTexture(const Texture* actual, const Texture* expected) {
    REQUIRE((actual != nullptr) == (expected != nullptr));
    if (!actual) {
        return;
    }
    const auto* left = dynamic_cast<const test::FakeTexture*>(actual);
    const auto* right = dynamic_cast<const test::FakeTexture*>(expected);
    REQUIRE(left);
    REQUIRE(right);
    CHECK(left->width() == right->width());
    CHECK(left->height() == right->height());
    CHECK((left->pixels == right->pixels));
}

void checkDraws(Fixture& actual, Fixture& expected) {
    for (auto* fixture : {&actual, &expected}) {
        fixture->device.draws.clear();
        fixture->items.draw(fixture->device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    }
    REQUIRE_FALSE(expected.device.draws.empty());
    REQUIRE(actual.device.draws.size() == expected.device.draws.size());
    for (usize i = 0; i < expected.device.draws.size(); ++i) {
        CAPTURE(i);
        const auto& left = actual.device.draws[i];
        const auto& right = expected.device.draws[i];
        checkTexture(left.texture, right.texture);
        auto state = left.state;
        checkTexture(state.nextTexture, right.state.nextTexture);
        checkTexture(state.lightmap, right.state.lightmap);
        checkTexture(state.maskedTexture, right.state.maskedTexture);
        state.nextTexture = right.state.nextTexture;
        state.lightmap = right.state.lightmap;
        state.maskedTexture = right.state.maskedTexture;
        CHECK(state == right.state);
        CHECK(left.transform == right.transform);
        REQUIRE(left.vertices.size() == right.vertices.size());
        for (usize vertex = 0; vertex < right.vertices.size(); ++vertex) {
            CHECK(left.vertices[vertex].position == right.vertices[vertex].position);
            CHECK(left.vertices[vertex].uv == right.vertices[vertex].uv);
            CHECK(left.vertices[vertex].uv2 == right.vertices[vertex].uv2);
            CHECK(left.vertices[vertex].color == right.vertices[vertex].color);
        }
    }
}

TEST_CASE("prepared pickup births use native record floor flight and particle seed rules",
          "[prepared-pickup-birth]") {
    const s32 rate = GENERATE(30, 60);
    const s32 floor = GENERATE(0, 1, 2);
    CAPTURE(rate, floor);
    Fixture native("prepared-birth-native", floor);
    Fixture replay("prepared-birth-replay", floor);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    for (s32 tick = 0; tick < 5; ++tick) {
        native.step(seconds);
        replay.step(seconds);
    }
    native.items.capturePresentation();
    replay.items.capturePresentation();
    const auto clocks = replay.clock();

    REQUIRE(native.items.place(native.device, "COIN", {-5, 2, 0}, native.world()));
    REQUIRE(replay.appended(replay.items.place(replay.device, "COIN", {-5, 2, 0},
                                               replay.world())) == std::optional<usize>{1});
    REQUIRE(native.items.placeRecord(native.device, 1, {5, 2, 0}, native.world(), 99));
    REQUIRE(replay.appended(replay.items.placeRecord(replay.device, 1, {5, 2, 0}, replay.world(),
                                                     99)) == std::optional<usize>{2});
    REQUIRE(native.items.placeRecord(native.device, 2, {0, 2, 0}, native.world(), -9));
    REQUIRE(replay.appended(replay.items.placeRecord(replay.device, 2, {0, 2, 0}, replay.world(),
                                                     -9)) == std::optional<usize>{3});
    REQUIRE(native.items.throwItem(native.device, "COIN", {2, 6, 3}, {3, 2, 1}, native.world(),
                                   0.4f, 17.0f));
    REQUIRE(replay.appended(replay.items.throwItem(replay.device, "COIN", {2, 6, 3}, {3, 2, 1},
                                                   replay.world(), 0.4f, 17.0f)) ==
            std::optional<usize>{4});
    // The births bind their figures; what follows creates and uploads nothing.
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    const auto& born = replay.items;
    REQUIRE(born.size() == 5);     // amount 99 is one native record, not 99 new items
    CHECK(born.item(1).info == 0); // first matching name, not the later duplicate
    CHECK(born.item(1).value == 50);
    CHECK(born.item(2).info == 1);
    CHECK(born.item(2).value == 99);
    CHECK(born.item(3).value == 11);            // nonpositive override uses the record
    CHECK_FALSE(born.item(3).player.playing()); // zero-sequence archive tree
    CHECK(born.item(4).position == Vec3{2, 6, 3});
    CHECK(born.item(4).strength == 17);
    CHECK(born.item(4).thrown == (floor != 0));
    CHECK(born.item(4).velocity == (floor == 0 ? Vec3{0} : Vec3{3, 2, 1}));
    CHECK(born.item(1).position.y == (floor == 0 ? 2.0f : PlacedItems::kFloorLift));
    CHECK(born.item(1).floor.has_value() == (floor == 2));
    CHECK_FALSE(born.item(4).floor);
    CHECK_FALSE(born.item(1).presentationCaptured);
    CHECK(replay.clock() == clocks);
    const auto particles = born.item(1).particles.checkpoint();
    REQUIRE(particles.field.entries.size() == 2);
    CHECK(particles.field.entries[0].emitter.random == 3);
    CHECK(particles.field.entries[1].emitter.random == 4);
    CHECK(particles == born.item(2).particles.checkpoint());
    CHECK(particles == born.item(4).particles.checkpoint());
    CHECK(replay.digest() == native.digest());

    bool emitted = false;
    bool landed = false;
    for (s32 tick = 0; tick < rate * 3; ++tick) {
        CAPTURE(tick);
        if (floor == 2 && tick == rate) {
            const auto moved = glm::translate(Mat4{1}, Vec3{1, 1, 0});
            native.collision.setObjectTransform(0, moved);
            replay.collision.setObjectTransform(0, moved);
        }
        native.step(seconds);
        replay.step(seconds);
        CHECK(replay.digest() == native.digest());
        emitted |=
            !native.items.item(1).particles.checkpoint().field.entries[0].emitter.particles.empty();
        landed |= !native.items.item(4).thrown;
        if (tick % 13 == 0) {
            checkDraws(replay, native);
            CHECK(replay.digest() == native.digest());
        }
    }
    CHECK(emitted);
    CHECK(landed);
    CHECK(replay.device.texturesCreated == textures);
    CHECK(replay.device.textureUpdates == uploads);
}

TEST_CASE("prepared carried pickup releases retain native no-op visibility and rewind semantics",
          "[prepared-pickup-birth]") {
    const s32 rate = GENERATE(30, 60);
    const bool collision = GENERATE(false, true);
    const bool taken = GENERATE(false, true);
    CAPTURE(rate, collision, taken);
    Fixture native("prepared-release-native", collision ? 2 : 0);
    Fixture replay("prepared-release-replay", collision ? 2 : 0);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    const auto position = native.items.item(0).position;
    REQUIRE(native.items.claim(position, 1, 2) == std::optional<usize>{0});
    REQUIRE(replay.items.claim(position, 1, 2) == std::optional<usize>{0});
    if (taken) {
        native.items.discard(0);
        replay.items.discard(0);
    }
    PlacedItems::LocalSnapshot before;
    replay.items.saveLocal(before);
    const u32 continuity = replay.items.item(0).continuity;
    const auto textures = replay.device.texturesCreated;
    REQUIRE(native.items.release(0, {0, 5, 0}, {3, 4, 0}, native.world(), 0.25f));
    REQUIRE(replay.items.release(0, {0, 5, 0}, {3, 4, 0}, replay.world(), 0.25f));
    CHECK(replay.digest() == native.digest());
    CHECK(replay.items.item(0).visible == !taken);
    CHECK(replay.items.item(0).continuity == continuity + 1);
    CHECK_FALSE(replay.items.item(0).carried);
    CHECK_FALSE(replay.items.item(0).floor);
    for (s32 tick = 0; tick < rate; ++tick) {
        native.step(seconds);
        replay.step(seconds);
        CHECK(replay.digest() == native.digest());
    }
    PlacedItems::LocalSnapshot continued;
    replay.items.saveLocal(continued);
    CHECK_FALSE(native.items.release(0, {}, {}, native.world(), 0));
    CHECK_FALSE(replay.items.release(0, {}, {}, replay.world(), 0));
    CHECK_FALSE(replay.items.release(999, {}, {}, replay.world(), 0));
    CHECK(holds(replay.items, continued));
    REQUIRE(replay.items.restoreLocal(before));
    CHECK(holds(replay.items, before));
    REQUIRE(replay.items.release(0, {0, 5, 0}, {3, 4, 0}, replay.world(), 0.25f));
    for (s32 tick = 0; tick < rate; ++tick) {
        replay.step(seconds);
    }
    CHECK(holds(replay.items, continued));
    CHECK(replay.device.texturesCreated == textures);
}

TEST_CASE("prepared pickup births and release failures leave the complete native owner intact",
          "[prepared-pickup-birth]") {
    Fixture fixture("prepared-birth-atomic", 2);
    fixture.step(1.0f / 30);
    PlacedItems::LocalSnapshot before;
    fixture.items.saveLocal(before);
    const auto textures = fixture.device.texturesCreated;
    CHECK_FALSE(fixture.items.place(fixture.device, "MISSING", {}, fixture.world()));
    CHECK_FALSE(fixture.items.place(fixture.device, "UNKNOWN", {}, fixture.world()));
    // Death is not a pickup.
    CHECK_FALSE(fixture.items.placeRecord(fixture.device, 4, {}, fixture.world()));
    CHECK_FALSE(fixture.items.placeRecord(fixture.device, -1, {}, fixture.world()));
    CHECK_FALSE(fixture.items.placeRecord(fixture.device, 99, {}, fixture.world()));
    CHECK(holds(fixture.items, before));

    // A birth rewinds like any other change of the owner.
    REQUIRE(fixture.appended(fixture.items.throwItem(fixture.device, "COIN", {0, 5, 0}, {1, 2, 3},
                                                     fixture.world(), 0.2f)) ==
            std::optional<usize>{1});
    fixture.step(1.0f / 30);
    PlacedItems::LocalSnapshot first;
    fixture.items.saveLocal(first);
    REQUIRE(fixture.items.restoreLocal(before));
    CHECK(holds(fixture.items, before));
    REQUIRE(fixture.appended(fixture.items.throwItem(fixture.device, "COIN", {0, 5, 0}, {1, 2, 3},
                                                     fixture.world(), 0.2f)) ==
            std::optional<usize>{1});
    fixture.step(1.0f / 30);
    CHECK(holds(fixture.items, first));

    REQUIRE(fixture.items.claim(fixture.items.item(0).position, 1, 2));
    CHECK(fixture.device.texturesCreated == textures);
}
} // namespace
