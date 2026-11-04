#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/level/ExitPortals.h"
#include "game/level/LevelCatalog.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"
#include "game/level/TowerAccess.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The portals' native digest over the stage they were bound to, so two loads can be
 * compared. */
NativeStateDigest::Digest digestOf(const ExitPortals& portals, const ItemArchive& items,
                                   const WorldLayout& layout, const WorldCollision* collision,
                                   const RenderDevice& device) {
    NativeStateResources resources;
    test::registerItemStage(resources, items, layout, collision, device);
    return test::liveDigest(portals, resources);
}

struct PortalFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    LevelCatalog catalog;
    ItemArchive items;
    ExitPortals portals;
    std::vector<ClassProgress> progress{1};
    TowerAccess access{progress};

    explicit PortalFixture(std::string_view name, bool artwork = true) {
        const auto directory = test::sampleLevel(name);
        std::filesystem::create_directories(directory / "wdata");
        writeTextFile(directory / "wdata/TOWN.json", R"({"realm":7,"prefix":"levelG",
          "levels":[{"name":"G1","title":"Town"},{"name":"G2","title":"Farm"}]})");
        writeTextFile(directory / "wdata/SECRET.json", R"({"realm":12,"prefix":"levelS",
          "levels":[{"name":"S1","title":"Secret"}]})");
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4102}],
          "itemInfos":[{"type":9,"name":"EXIT_PORTAL","radius":3},
                       {"type":9,"subtype":50,"name":"SECRET_ICON","radius":1}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[0,0,0,0,103,49,0,0,0,0,0,0]},
            {"info":1,"position":[20,0,0],"params":[0,0,0,0,83,49,0,0,0,0,0,0]},
            {"info":0,"position":[-20,0,0],"params":[0,0,0,0,103,50,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
          "trees":[{"name":"EXIT_PORTAL",
            "nodes":[{"name":"BODY","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30},
              {"name":"READY","frames":4,"frameRate":30},
              {"name":"ACTIVE1","frames":4,"frameRate":30},
              {"name":"ACTIVE2","frames":4,"frameRate":30},
              {"name":"ACTIVE3","frames":4,"frameRate":30}]},
            {"name":"SECRET_ICON","nodes":[{"name":"ICON","object":"WALL","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":9,"frameRate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(catalog.load(directory));
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
        REQUIRE(portals.bind(device, layout, AssetScope(items), catalog, &collision, &access));
        REQUIRE(portals.size() == 3);
        if (!items.loaded()) {
            for (usize index = 0; index < portals.size(); ++index) {
                CHECK(portals.portal(index).icon.presentation().archive == nullptr);
            }
        }
    }
    NativeStateDigest::Digest digest() const {
        return digestOf(portals, items, layout, &collision, device);
    }
};

TEST_CASE("native portal history rewinds contacts waiting events secret icons and departure",
          "[local-rewind][exit-portals-local-rewind]") {
    const bool artwork = GENERATE(false, true);
    PortalFixture native("portal-local-native", artwork);
    PortalFixture replay("portal-local-replay", artwork);
    std::array<ExitPortals::LocalSnapshot, 16> history;
    WorldCollision::LocalSnapshot floor;
    const auto* model = &replay.portals.portal(0).model;
    const auto uploads = replay.device.texturesCreated;
    const auto step = [](PortalFixture& fixture, u32 tick, bool wrong) {
        const f32 height = wrong ? -8.0f : static_cast<f32>(tick % 40) * 0.05f;
        fixture.collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, height, 0}));
        std::array party{
            PortalVisitor{{0, height, 0}, 0.75f, 3, true},
            PortalVisitor{{tick % 120 < 70 ? 10.0f : 0.0f, height, 0}, 0.75f, 1, tick % 9 != 0}};
        if (wrong || tick == 320) {
            party[0].position.x = 20;
        }
        fixture.portals.setAlpha("g1", wrong ? 0.1f : 0.8f);
        if (wrong || tick == 325) {
            fixture.portals.consume(wrong ? 0 : 1);
        }
        const auto departure = fixture.portals.update(1, 1.0f / 60, party);
        if (wrong || tick % 7 == 0) {
            fixture.portals.takeWaiting();
        }
        return departure;
    };
    const auto compareDraws = [&] {
        native.device.draws.clear();
        replay.device.draws.clear();
        native.portals.draw(native.device, Mat4{1}, {});
        replay.portals.draw(replay.device, Mat4{1}, {});
        REQUIRE(replay.device.draws.size() == native.device.draws.size());
        for (usize i = 0; i < native.device.draws.size(); ++i) {
            CHECK(replay.device.draws[i].vertices == native.device.draws[i].vertices);
            CHECK(replay.device.draws[i].transform == native.device.draws[i].transform);
            CHECK(replay.device.draws[i].state.uvOffset == native.device.draws[i].state.uvOffset);
            CHECK(replay.device.draws[i].state.blend == native.device.draws[i].state.blend);
        }
    };
    u32 tick = 0;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick, artwork);
        auto& saved = history[window % history.size()];
        replay.portals.saveLocal(saved);
        replay.collision.saveLocal(floor);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.portals.restoreLocal(saved));
        CHECK(replay.digest() == native.digest());
        compareDraws(); // No simulation update may be needed to repair the restored picture.
        for (u32 i = 0; i < distance; ++i) {
            CHECK(step(replay, tick + i, false) == step(native, tick + i, false));
            CHECK(replay.digest() == native.digest());
            compareDraws();
        }
        tick += distance;
    }
    CHECK(replay.portals.takeWaiting() == native.portals.takeWaiting());
    CHECK(&replay.portals.portal(0).model == model);
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.portals.restoreLocal(history[0]));
    replay.bind();
    CHECK_FALSE(replay.portals.restoreLocal(history[0]));
    replay.portals.saveLocal(history[0]);
    replay.portals.clear();
    CHECK_FALSE(replay.portals.restoreLocal(history[0]));
}

TEST_CASE("two portal loads keep native phases floor carry texture clocks and wait queues alike",
          "[rollback][exit-portals-local-rewind]") {
    const s32 rate = GENERATE(30, 60);
    const bool artwork = GENERATE(false, true);
    CAPTURE(rate, artwork);
    PortalFixture host("portal-checkpoint-host", artwork);
    PortalFixture guest("portal-checkpoint-guest", artwork);
    const s32 ticks = 60 / rate;
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    std::array party{PortalVisitor{{0, 0, 0}, 0.75f, 3, true},
                     PortalVisitor{{10, 0, 10}, 0.75f, 1, true}};
    for (s32 frame = 0; frame < rate; ++frame) {
        host.portals.update(ticks, seconds, party);
        guest.portals.update(ticks, seconds, party);
    }
    host.portals.setAlpha("g1", 0.4f);
    guest.portals.setAlpha("g1", 0.4f);
    REQUIRE(host.portals.portal(0).action == ExitPortals::kWaiting);
    REQUIRE(host.portals.portal(2).shut);
    if (artwork) {
        REQUIRE(host.portals.portal(0).player.playing());
        const auto icon = host.portals.portal(1).icon.checkpoint();
        REQUIRE(icon);
        REQUIRE(icon->textures.frame > 0);
    }
    const auto textures = guest.device.texturesCreated;
    const auto uploads = guest.device.textureUpdates;
    CHECK(guest.digest() == host.digest());
    const auto waiting = host.portals.takeWaiting();
    REQUIRE_FALSE(waiting.empty());
    CHECK(guest.portals.takeWaiting() == waiting);
    bool departed = false;
    for (s32 frame = 0; frame < rate * 2; ++frame) {
        const f32 height = static_cast<f32>(frame) / static_cast<f32>(rate);
        const auto transform = glm::translate(Mat4{1}, Vec3{0, height, 0});
        host.collision.setObjectTransform(0, transform);
        guest.collision.setObjectTransform(0, transform);
        party[0].position.y = party[1].position.y = height;
        party[1].position.x = party[1].position.z = 0;
        const auto expected = host.portals.update(ticks, seconds, party);
        const auto actual = guest.portals.update(ticks, seconds, party);
        CHECK(actual == expected);
        CHECK(guest.digest() == host.digest());
        CHECK(guest.portals.takeWaiting() == host.portals.takeWaiting());
        departed = departed || actual.has_value();
    }
    REQUIRE(departed);
    CHECK(guest.device.texturesCreated == textures);
    CHECK(guest.device.textureUpdates == uploads);
    CHECK(guest.portals.portal(0).support == 0);
    CHECK(guest.portals.portal(0).position.y > ExitPortals::kFloorLift);
}

TEST_CASE("secret portal digests keep the actual toucher and consumed icon without travel",
          "[rollback][exit-portals-local-rewind]") {
    PortalFixture fixture("portal-checkpoint-secret");
    PortalFixture other("portal-checkpoint-secret-other");
    const std::array touching{PortalVisitor{{20.5f, 0, 0}, 0.75f, 2, true}};
    REQUIRE(fixture.portals.update(1, 1.0f / 60, touching) == 1);
    fixture.portals.consume(1);
    REQUIRE(fixture.portals.portal(1).departurePosition == touching[0].position);
    REQUIRE(fixture.portals.portal(1).consumed);
    const auto textures = other.device.texturesCreated;
    REQUIRE(other.portals.update(1, 1.0f / 60, touching) == 1);
    other.portals.consume(1);
    CHECK(other.digest() == fixture.digest());
    CHECK(other.device.texturesCreated == textures);
    CHECK_FALSE(other.portals.update(1, 1.0f / 60, touching));
    CHECK(other.portals.portal(1).icon.checkpoint() == fixture.portals.portal(1).icon.checkpoint());
}

TEST_CASE("native tower portals keep open and EXIT_OFF resources independently in two loads",
          "[rollback][exit-portals-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive hostItems;
    ItemArchive guestItems;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    REQUIRE(catalog.load(root));
    REQUIRE(hostItems.load(root / "ITEMS/LEVELL"));
    REQUIRE(guestItems.load(root / "ITEMS/LEVELL"));
    std::vector<ClassProgress> progress(1);
    progress[0].levels.recordBeaten(7, 0);
    const TowerAccess access{progress};
    ExitPortals host;
    ExitPortals guest;
    REQUIRE(host.bind(device, layout, AssetScope(hostItems), catalog, nullptr, &access));
    REQUIRE(guest.bind(device, layout, AssetScope(guestItems), catalog, nullptr, &access));
    std::optional<usize> open;
    std::optional<usize> closed;
    for (usize index = 0; index < host.size(); ++index) {
        const auto& portal = host.portal(index);
        if (portal.model.bound() && !portal.secret) {
            (portal.shut ? closed : open) = index;
        }
    }
    REQUIRE(open);
    REQUIRE(closed);
    const std::array party{PortalVisitor{host.portal(*open).position, 0.75f, 2, true}};
    for (s32 tick = 0; tick < 21; ++tick) {
        host.update(1, 1.0f / 60, party);
        guest.update(1, 1.0f / 60, party);
    }
    host.setAlpha(host.portal(*closed).tag, 0.25f);
    guest.setAlpha(guest.portal(*closed).tag, 0.25f);
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    CHECK(digestOf(guest, guestItems, layout, nullptr, device) ==
          digestOf(host, hostItems, layout, nullptr, device));
    REQUIRE(guest.portal(*closed).model.bound());
    CHECK(guest.portal(*closed).archive == &guestItems);
    CHECK(guest.portal(*open).archive == &guestItems);
    for (s32 tick = 0; tick < 180; ++tick) {
        CAPTURE(tick);
        CHECK(guest.update(1, 1.0f / 60, party) == host.update(1, 1.0f / 60, party));
        CHECK(digestOf(guest, guestItems, layout, nullptr, device) ==
              digestOf(host, hostItems, layout, nullptr, device));
        CHECK(guest.takeWaiting() == host.takeWaiting());
    }
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}
} // namespace
