#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelTransporters.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether the pads' last presentation capture still holds the picture; false without art. */
bool presentationAdvanced(const LevelTransporters& pads) {
    const auto shown = pads.presentation(0);
    return shown.has_value() && shown->advanced;
}

struct TransportFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    ItemArchive items;
    LevelTransporters pads;
    static constexpr s32 kPlayers = 1;

    explicit TransportFixture(std::string_view name, bool artwork = true) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4102}],
          "itemInfos":[{"type":11,"name":"TRANS","radius":1.6,"height":2}],
          "itemInstances":[
            {"info":0,"position":[2,0,3],"params":[10,0,0,0,20,0,0,0]},
            {"info":0,"position":[-4,0,5],"params":[20,0,0,0,10,0,0,0]},
            {"info":0,"position":[-8,0,-8],"params":[30,0,0,0,30,0,0,0]},
            {"info":0,"minPlayers":2,"position":[8,0,8],"params":[20,0,0,0,10,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
          "trees":[{"name":"TRANS",
            "nodes":[{"name":"BODY","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
              "tracks":[{"node":0,"flags":16,"frames":[0,3],"values":[0,2]}]}]}]})");
        REQUIRE(layout.load(directory));
        if (artwork) {
            REQUIRE(items.load(directory));
        }
        collision.build(
            {CollisionTriangle{.vertices = {Vec3{-40, 0, -40}, Vec3{40, 0, 40}, Vec3{40, 0, -40}},
                               .object = 0},
             CollisionTriangle{.vertices = {Vec3{-40, 0, -40}, Vec3{-40, 0, 40}, Vec3{40, 0, 40}},
                               .object = 0}});
        collision.setMovingObjects(std::array<s32, 1>{0});
        bind();
    }
    void bind() {
        pads.bind(device, layout, AssetScope(items), kPlayers, &collision);
        REQUIRE(pads.size() == 3);
    }
    /** The pads' native digest over their own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        test::registerItemStage(resources, items, layout, &collision, device);
        return test::liveDigest(pads, resources.resources());
    }
};

void sameDraws(test::FakeRenderDevice& host, test::FakeRenderDevice& guest,
               const LevelTransporters& native, const LevelTransporters& restored, f32 alpha) {
    host.draws.clear();
    guest.draws.clear();
    native.draw(host, Mat4{1}, {}, alpha);
    restored.draw(guest, Mat4{1}, {}, alpha);
    REQUIRE(host.draws.size() == guest.draws.size());
    for (usize index = 0; index < host.draws.size(); ++index) {
        const auto& expected = host.draws[index];
        const auto& actual = guest.draws[index];
        CHECK(actual.vertices == expected.vertices);
        CHECK(actual.transform == expected.transform);
        CHECK(actual.state.depthTest == expected.state.depthTest);
        CHECK(actual.state.depthWrite == expected.state.depthWrite);
        CHECK(actual.state.blend == expected.state.blend);
        REQUIRE(actual.texture != expected.texture); // Independent native resource owners.
        const auto* expectedTexture = dynamic_cast<const test::FakeTexture*>(expected.texture);
        const auto* actualTexture = dynamic_cast<const test::FakeTexture*>(actual.texture);
        REQUIRE(expectedTexture);
        REQUIRE(actualTexture);
        CHECK(actualTexture->pixels == expectedTexture->pixels);
    }
}

TEST_CASE("native transporter history rewinds floor contacts animation and held presentation",
          "[local-rewind][transporters-local-rewind]") {
    const bool artwork = GENERATE(false, true);
    TransportFixture native("trans-local-native", artwork);
    TransportFixture replay("trans-local-replay", artwork);
    std::array<LevelTransporters::LocalSnapshot, 16> history;
    WorldCollision::LocalSnapshot floor;
    const auto* model = &replay.pads.pad(0).model;
    const auto uploads = replay.device.texturesCreated;
    const auto step = [](TransportFixture& fixture, u32 tick, bool wrong) {
        fixture.pads.capturePresentation();
        fixture.collision.setObjectTransform(
            0, glm::rotate(glm::translate(
                               Mat4{1}, Vec3{1, wrong ? -8.0f : static_cast<f32>(tick) * 0.01f, 2}),
                           static_cast<f32>(tick % 30) * 0.02f, Vec3{0, 1, 0}));
        fixture.collision.setSolid(0, tick % 3 != 0);
        fixture.pads.syncFloors(fixture.collision);
        if (wrong || tick % 5 != 0) {
            fixture.pads.animate(wrong ? 0.2f : 1.0f / 60);
        }
    };
    u32 tick = 0;
    for (u32 window = 0; window < 80; ++window) {
        CAPTURE(window, tick, artwork);
        auto& saved = history[window % history.size()];
        replay.pads.saveLocal(saved);
        replay.collision.saveLocal(floor);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.pads.restoreLocal(saved));
        CHECK(replay.digest() == native.digest());
        sameDraws(native.device, replay.device, native.pads, replay.pads, 0.35f);
        for (u32 i = 0; i < distance; ++i) {
            step(native, tick + i, false);
            step(replay, tick + i, false);
            CHECK(replay.digest() == native.digest());
            CHECK(replay.pads.contact(native.pads.pad(0).position, 0.5f, 2) == 0);
            for (f32 const alpha : {-1.0f, 0.0f, 0.35f, 1.0f}) {
                sameDraws(native.device, replay.device, native.pads, replay.pads, alpha);
            }
        }
        tick += distance;
    }
    CHECK(&replay.pads.pad(0).model == model);
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.pads.restoreLocal(history[0]));
    replay.bind();
    CHECK_FALSE(replay.pads.restoreLocal(history[0]));
    replay.pads.saveLocal(history[0]);
    replay.pads.clear();
    CHECK_FALSE(replay.pads.restoreLocal(history[0]));
}

TEST_CASE("two transporter loads keep native floor clock and loop presentation alike",
          "[rollback][transporters-local-rewind]") {
    const s32 rate = GENERATE(30, 60);
    const bool artwork = GENERATE(false, true);
    CAPTURE(rate, artwork);
    TransportFixture host("trans-checkpoint-host", artwork);
    TransportFixture guest("trans-checkpoint-guest", artwork);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    REQUIRE(host.pads.pad(0).support == 0);
    for (s32 tick = 0; tick < 5; ++tick) {
        host.pads.animate(seconds);
        guest.pads.animate(seconds);
    }
    // Fractional clock credit, not a displayed-frame reconstruction.
    host.pads.animate(0.007f);
    guest.pads.animate(0.007f);
    CHECK(host.pads.pad(0).destination == 1);
    CHECK(host.pads.pad(1).destination == 0);
    CHECK_FALSE(host.pads.pad(2).destination);
    const auto textures = guest.device.texturesCreated;
    const auto uploads = guest.device.textureUpdates;
    const auto* address = &guest.pads.pad(0);
    CHECK(guest.digest() == host.digest());
    if (artwork) {
        REQUIRE(host.pads.pad(0).animation.playing());
        REQUIRE(host.pads.pad(0).pose.posed());
        REQUIRE_FALSE(host.device.texturesCreated == 0);
    }
    bool looped = false;
    u64 generation = host.pads.pad(0).animation.generation();
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(tick);
        const Mat4 moved =
            glm::rotate(glm::translate(Mat4{1}, Vec3{1, static_cast<f32>(tick) / 10, 2}),
                        static_cast<f32>(tick) / 20, Vec3{0, 1, 0});
        host.collision.setSolid(0, tick % 3 != 0);
        guest.collision.setSolid(0, tick % 3 != 0);
        host.collision.setObjectTransform(0, moved);
        guest.collision.setObjectTransform(0, moved);
        host.pads.syncFloors(host.collision);
        guest.pads.syncFloors(guest.collision);
        host.pads.animate(seconds);
        guest.pads.animate(seconds);
        CHECK(guest.digest() == host.digest());
        CHECK(guest.pads.contact(host.pads.pad(0).position, 0.5f, 2) == 0);
        looped |= generation != host.pads.pad(0).animation.generation();
        generation = host.pads.pad(0).animation.generation();
        for (const f32 alpha : {-1.0f, 0.0f, 0.35f, 1.0f}) {
            sameDraws(host.device, guest.device, host.pads, guest.pads, alpha);
        }
        if (tick % 7 == 0) {
            host.pads.capturePresentation();
            guest.pads.capturePresentation();
            REQUIRE_FALSE(presentationAdvanced(host.pads));
            sameDraws(host.device, guest.device, host.pads, guest.pads, 0.1f);
        }
    }
    CHECK(looped == artwork);
    CHECK(&guest.pads.pad(0) == address);
    CHECK(guest.device.texturesCreated == textures);
    CHECK(guest.device.textureUpdates == uploads);
}

TEST_CASE("transporter restore preserves historical placement before its next native carry",
          "[rollback][transporters-local-rewind]") {
    TransportFixture host("trans-checkpoint-lag-host");
    TransportFixture guest("trans-checkpoint-lag-guest");
    const auto placement = glm::translate(Mat4{1}, Vec3{4, 2, 6});
    host.collision.setObjectTransform(0, placement);
    guest.collision.setObjectTransform(0, placement);
    host.pads.syncFloors(host.collision);
    guest.pads.syncFloors(guest.collision);
    host.pads.animate(0.025f);
    guest.pads.animate(0.025f);
    LevelTransporters::LocalSnapshot baseline;
    host.pads.saveLocal(baseline);
    const Mat4 supportLocal = host.pads.pad(0).supportLocal;
    const auto later = glm::rotate(glm::translate(Mat4{1}, Vec3{9, 11, 13}), 0.75f, Vec3{0, 1, 0});
    // Scripted world motion can leave pads at their preceding carry phase.
    host.collision.setObjectTransform(0, later);
    guest.collision.setObjectTransform(0, later);
    guest.collision.setSolid(0, false);
    host.pads.syncFloors(host.collision); // The branch being undone carries the pads on.
    REQUIRE(host.pads.restoreLocal(baseline));
    CHECK(host.digest() == guest.digest());
    CHECK(host.pads.pad(0).transform != later * supportLocal);
    sameDraws(guest.device, host.device, guest.pads, host.pads, 0.4f);
    host.pads.syncFloors(host.collision);
    guest.pads.syncFloors(guest.collision);
    CHECK(host.digest() == guest.digest());
    CHECK(host.pads.pad(0).transform == later * supportLocal);
}

TEST_CASE("all thirty six native transporter pads across seven levels advance alike in two loads",
          "[rollback][transporters-local-rewind][assets]") {
    const s32 rate = GENERATE(30, 60);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize count = 0;
    usize levels = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            WorldLayout census;
            REQUIRE(census.load(root / ref->directory));
            bool padsPresent = false;
            for (const auto& info : census.itemInfos()) {
                padsPresent |= info.type == LevelTransporters::kItemType;
            }
            if (!padsPresent) {
                continue;
            }
            CAPTURE(name, rate);
            test::FakeRenderDevice device;
            LevelWorld hostWorld;
            LevelWorld guestWorld;
            REQUIRE(hostWorld.load(device, root, *ref));
            REQUIRE(guestWorld.load(device, root, *ref));
            LevelTransporters host;
            LevelTransporters guest;
            host.bind(device, hostWorld.layout(), hostWorld.itemScope(), 1, &hostWorld.collision());
            guest.bind(device, guestWorld.layout(), guestWorld.itemScope(), 1,
                       &guestWorld.collision());
            const auto digestOf = [&device](const LevelTransporters& pads, LevelWorld& world) {
                NativeStateResources resources;
                test::registerStage(resources, world, device);
                return test::liveDigest(pads, resources.resources());
            };
            const f32 seconds = 1.0f / static_cast<f32>(rate);
            for (s32 tick = 0; tick < 13; ++tick) {
                host.animate(seconds);
                guest.animate(seconds);
            }
            host.animate(0.007f);
            guest.animate(0.007f);
            const auto textures = device.texturesCreated;
            const auto uploads = device.textureUpdates;
            CHECK(digestOf(guest, guestWorld) == digestOf(host, hostWorld));
            for (s32 tick = 0; tick < rate; ++tick) {
                host.animate(seconds);
                guest.animate(seconds);
                CHECK(digestOf(host, hostWorld) == digestOf(guest, guestWorld));
            }
            CHECK(device.texturesCreated == textures);
            CHECK(device.textureUpdates == uploads);
            for (usize index = 0; index < host.size(); ++index) {
                CAPTURE(index);
                CHECK(host.pad(index).model.bound());
                CHECK(guest.pad(index).animation.playing());
            }
            count += host.size();
            ++levels;
        }
    }
    CHECK(count == 36);
    CHECK(levels == 7);
}
} // namespace
