#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/party/PortalDeparture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct DreamcastTravelFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;

    explicit DreamcastTravelFixture(std::string_view name) {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
            SKIP("Dreamcast extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.rom");
        auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
        map.textures =
            formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::dataDirectory());
    }

    void admit(std::span<PartyMember> party, std::string_view destination) {
        const auto portals = level->adapter().dreamcastPortals();
        const auto found = std::ranges::find(portals, destination, &LegendsDreamcastPortal::map);
        REQUIRE(found != portals.end());
        level->initializeParty(party);
        std::array<PlayerActor, 4> actors;
        std::vector<PlayerActor*> visitors;
        for (const auto& member : party) {
            if (!member.fallen) {
                auto& actor = actors[static_cast<usize>(member.player)];
                actor.spawn(member.player, member.save, nullptr, found->position, 0);
                visitors.push_back(&actor);
            }
        }
        for (s32 tick = 0; tick < 240 && !level->destination(); ++tick) {
            level->capturePresentation();
            level->update(1.0f / 60, false);
            level->updateParty(visitors, 1.0f / 60, false);
        }
        REQUIRE(level->destination());
        REQUIRE(*level->destination() == destination);
        // Null means the shared sinking departure owns this transition. It
        // must not accidentally opt into N64's spin-and-shrink presentation.
        REQUIRE_FALSE(level->departure());
        for (auto& member : party) {
            if (!member.fallen) {
                member.save = actors[static_cast<usize>(member.player)].save();
            }
        }
    }
};

std::array<PartyMember, 3> travelParty() {
    std::array<PartyMember, 3> result;
    for (usize i = 0; i < result.size(); ++i) {
        auto& member = result[i];
        member.player = static_cast<s32>(i);
        member.slot = i + 3;
        member.save.character = static_cast<s32>(i);
        member.save.gold = 1000;
        member.save.progress().health = 9000;
        member.save.progress().legendsProgress(LegendsPlatform::N64).recordCompleted("mount1");
    }
    result[2].fallen = true;
    return result;
}

TEST_CASE("Dreamcast common departure records the completed stage without a native override",
          "[legends][dreamcast-travel][assets]") {
    DreamcastTravelFixture fixture{"levelb1"};
    auto party = travelParty();
    const auto entry = party;
    fixture.level->completeTravel(party);
    for (usize i = 0; i < party.size(); ++i) {
        CHECK(party[i].save == entry[i].save);
    }
    fixture.admit(party, "levelt1");
    const auto admitted = party;

    SECTION("completed common transition awards only living party members") {
        TextureSet textures;
        PortalDeparture common;
        common.begin(fixture.device, textures);
        common.update(PortalDeparture::kTicks);
        REQUIRE(common.finished());
        // This is the callback made only after the app's common departure and
        // covering transition have finished and the replacement level loaded.
        fixture.level->completeTravel(party);
        for (usize i = 0; i < party.size(); ++i) {
            auto expected = admitted[i].save;
            if (!party[i].fallen) {
                REQUIRE(expected.progress()
                            .legendsProgress(LegendsPlatform::Dreamcast)
                            .recordCompleted("levelb1"));
            }
            CHECK(party[i].save == expected);
            CHECK(party[i].slot == admitted[i].slot);
            CHECK_FALSE(party[i]
                            .save.progress()
                            .legendsProgress(LegendsPlatform::Dreamcast)
                            .hasCompleted("levelt1"));
        }
        const auto completed = party;
        fixture.level->completeTravel(party);
        for (usize i = 0; i < party.size(); ++i) {
            CHECK(party[i].save == completed[i].save);
        }
    }
    SECTION("canceled load cannot record completion") {
        REQUIRE(fixture.level->cancelDeparture());
        fixture.level->completeTravel(party);
        for (usize i = 0; i < party.size(); ++i) {
            CHECK(party[i].save == admitted[i].save);
        }
    }
}

TEST_CASE("Dreamcast Tower departure never grants level completion",
          "[legends][dreamcast-travel][assets]") {
    DreamcastTravelFixture fixture{"levelt1"};
    auto party = travelParty();
    fixture.admit(party, "levelb1");
    const auto admitted = party;
    fixture.level->completeTravel(party);
    for (usize i = 0; i < party.size(); ++i) {
        CHECK(party[i].save == admitted[i].save);
    }
}

TEST_CASE("Dreamcast departure holds its camera while the common transport sinks the party",
          "[legends][dreamcast-travel][dreamcast-camera][assets]") {
    for (const auto* const name : {"levelt1", "levelb1"}) {
        DYNAMIC_SECTION(name) {
            DreamcastTravelFixture fixture{name};
            const bool tower = std::string_view{name} == "levelt1";
            const std::string_view destination = tower ? "levelb1" : "levelt1";
            const auto portals = fixture.level->adapter().dreamcastPortals();
            const auto exit = std::ranges::find(portals, destination, &LegendsDreamcastPortal::map);
            REQUIRE(exit != portals.end());
            std::array subjects{CameraSubject{exit->position, exit->position + Vec3{0, 3, 0}, 3}};
            fixture.level->resetCamera(subjects);
            subjects[0].feet.x += 4;
            subjects[0].follow.x += 4;
            // Leave a genuine previous/current pair at admission; varying
            // render alpha must not keep moving through that pair afterward.
            for (s32 tick = 0; tick < 3; ++tick) {
                fixture.level->followCamera(subjects, 1.0f / 60);
            }
            auto party = travelParty();
            fixture.admit(party, destination);
            const auto admitted = fixture.level->camera();
            const auto attention = fixture.level->cameraAttention();
            REQUIRE(admitted);
            REQUIRE(attention);
            for (s32 tick = 0; tick < 180; ++tick) {
                subjects[0].feet.y -= .1f;
                subjects[0].follow.y -= .1f;
                fixture.level->followCamera(subjects, 1.0f / 60);
                for (const f32 alpha : {0.0f, .25f, .75f, 1.0f}) {
                    CHECK(fixture.level->camera(alpha) == admitted);
                }
                CHECK(fixture.level->cameraAttention() == attention);
            }
            REQUIRE(fixture.level->cancelDeparture());
            subjects[0] = {exit->position + Vec3{8, 0, 0}, exit->position + Vec3{8, 3, 0}, 3};
            for (s32 tick = 0; tick < 90; ++tick) {
                fixture.level->followCamera(subjects, 1.0f / 30);
            }
            CHECK(fixture.level->camera() != admitted);
            CHECK(fixture.level->cameraAttention() != attention);
        }
    }
}

TEST_CASE("Dreamcast Valley of Fire views its authored transmitters from the native side",
          "[legends][dreamcast-travel][dreamcast-camera][assets]") {
    // NOLINTNEXTLINE(misc-const-correctness): the level changes behind MSVC's unique_ptr
    DreamcastTravelFixture fixture{"levelb1"};
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    const auto native = formats::WorldFile::parse(readFile(directory / "worlds.rom"));
    const auto markers = fixture.level->adapter().dreamcastCameraMarkers();
    usize markerIndex = 0;
    for (const auto& raw : native.locators) {
        if (raw.kind != LocatorKind::CameraStart && raw.kind != LocatorKind::CameraGame) {
            continue;
        }
        REQUIRE(markerIndex < markers.size());
        const auto& marker = markers[markerIndex++];
        CHECK(marker.kind == raw.kind);
        if (marker.kind == LocatorKind::CameraStart) {
            continue; // Entry is an eye, not a gameplay attention transmitter.
        }
        const CameraRange range{20, 20, 0, Vec3{0}, Vec3{0}};
        const std::array party{CameraSubject{marker.position, marker.position + Vec3{0, 3, 0}, 3}};
        LegendsDreamcastLevelCamera camera;
        camera.reset(party, std::span{&marker, 1}, range, false);
        // Independently reconstruct the native orbit: SetLocators adds pi,
        // CreateYPRMatrix transforms +Z, then 8c0774a0 adds it to attention.
        const f32 yaw = raw.rotation.y + kPi;
        const f32 pitch = raw.rotation.x;
        const Vec3 orbit{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                         std::cos(pitch) * std::cos(yaw)};
        CHECK(glm::distance(camera.camera().position, marker.position + orbit * 20.0f) < .001f);
        CHECK(glm::dot(camera.camera().forward(), -orbit) > .99999f);
    }
    REQUIRE(markerIndex == markers.size());
    REQUIRE(markerIndex > 2);

    // Exercise the real scene integration after the native entrance completes.
    // Near the first transmitter, B1 looks towards -Z from the +Z side.
    fixture.level->resetCamera(std::array{CameraSubject{{-2, 50, 52}, {-2, 53, 52}, 3}});
    for (s32 tick = 0; tick < 240 && !fixture.level->opponentsReady(); ++tick) {
        fixture.level->followCamera(std::array{CameraSubject{{-2, 50, 52}, {-2, 53, 52}, 3}},
                                    1.0f / 30);
    }
    REQUIRE(fixture.level->opponentsReady());
    const auto view = fixture.level->camera();
    const auto focus = fixture.level->cameraAttention();
    REQUIRE(view);
    REQUIRE(focus);
    CHECK(view->forward().z < -.9f);
    CHECK(view->position.z > focus->z);
    CHECK(view->position.y > focus->y);
}

TEST_CASE("Dreamcast B1 entrance gates admission and its bridge rails cannot collapse the eye",
          "[legends][dreamcast-travel][dreamcast-camera][assets]") {
    // NOLINTNEXTLINE(misc-const-correctness): the level changes behind MSVC's unique_ptr
    DreamcastTravelFixture fixture{"levelb1"};
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    const auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                         readFile(directory / "objects.rom"));
    REQUIRE_FALSE(map.starts.empty());
    const auto markers = fixture.level->adapter().dreamcastCameraMarkers();
    const auto entry = std::ranges::find(markers, LocatorKind::CameraStart, &WorldLocator::kind);
    REQUIRE(entry != markers.end());
    auto party = std::array{CameraSubject{map.starts.front().position,
                                          map.starts.front().position + Vec3{0, 12, 0}, 12}};
    CHECK_FALSE(fixture.level->opponentsReady());
    fixture.level->resetCamera(party);
    REQUIRE(fixture.level->camera());
    CHECK(glm::distance(fixture.level->camera()->position, entry->position) < .001f);
    CHECK_FALSE(fixture.level->opponentsReady());
    for (s32 tick = 0; tick < 88; ++tick) {
        fixture.level->followCamera(party, 1.0f / 60);
        CHECK(fixture.level->camera()->position == entry->position);
        CHECK_FALSE(fixture.level->opponentsReady());
        for (const f32 alpha : {0.0f, .5f, 1.0f}) {
            (void)fixture.level->camera(alpha);
        }
    }
    for (s32 tick = 0; tick < 480 && !fixture.level->opponentsReady(); ++tick) {
        fixture.level->followCamera(party, 1.0f / 60);
    }
    REQUIRE(fixture.level->opponentsReady());

    std::vector<CollisionTriangle> triangles;
    for (const auto& face : map.collision) {
        if (face.floor || face.wall) {
            triangles.push_back({face.normal, face.vertices, face.owner,
                                 (face.floor ? 4U : 0U) | (face.wall ? 2U : 0U)});
        }
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    fixture.level->bindCollision(collision);
    const auto left =
        std::ranges::find(map.objects, "B1POST7_ROOT", &formats::LegendsMapObject::name);
    const auto right =
        std::ranges::find(map.objects, "B1POST8_ROOT", &formats::LegendsMapObject::name);
    REQUIRE(left != map.objects.end());
    REQUIRE(right != map.objects.end());
    REQUIRE(std::ranges::any_of(
        map.objects, [](const auto& object) { return object.name.starts_with("B1ROPE"); }));
    // Walk the first bridge's actual authored posts, not a synthetic map. The
    // native transmitter always retains its twenty-unit orbit. Ground-level
    // attention rays through a rope/post must not drag the eye into the rail.
    Vec3 from = Vec3{right->transform[3]};
    Vec3 to = Vec3{left->transform[3]};
    from.y -= 2;
    to.y -= 2;
    for (const s32 direction : {1, -1}) {
        for (s32 frame = 0; frame <= 120; ++frame) {
            const f32 t = static_cast<f32>(direction > 0 ? frame : 120 - frame) / 120;
            Vec3 feet = glm::mix(from, to, t);
            if (const auto floor = collision.floorAt(feet, 5, 5)) {
                feet.y = floor->y;
            }
            party[0] = {feet, feet + Vec3{0, 12, 0}, 12};
            fixture.level->followCamera(party, 1.0f / 30);
            const auto camera = fixture.level->camera();
            const auto attention = fixture.level->cameraAttention();
            REQUIRE(camera);
            REQUIRE(attention);
            CHECK(std::abs(glm::distance(camera->position, *attention) - 20.0f) < .001f);
        }
    }
}

TEST_CASE("Dreamcast Tower ramp retains native framing instead of retracting into the player",
          "[legends][dreamcast-travel][dreamcast-camera][assets]") {
    // NOLINTNEXTLINE(misc-const-correctness): the level changes behind MSVC's unique_ptr
    DreamcastTravelFixture fixture{"levelt1"};
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelt1";
    const auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                         readFile(directory / "objects.rom"));
    REQUIRE(map.objects.size() > 627);
    REQUIRE(map.objects[627].name == "T1RAMP_STAI#00");
    std::vector<CollisionTriangle> triangles;
    for (const auto& face : map.collision) {
        if (face.floor || face.wall) {
            triangles.push_back({face.normal, face.vertices, face.owner,
                                 (face.floor ? 4U : 0U) | (face.wall ? 2U : 0U)});
        }
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    fixture.level->bindCollision(collision);
    auto party = std::array{CameraSubject{{61.51f, -5.99f, -2.95f}, {61.51f, 6.01f, -2.95f}, 12}};
    const Vec3 hub = fixture.level->adapter().dreamcastCameraMarkers().front().position;
    LegendsDreamcastTowerCamera native;
    native.reset(party, hub);
    fixture.level->resetCamera(party);
    // Independently evaluated 8c07e580 radial formula at the reported feet.
    CHECK(glm::distance(native.camera().position, Vec3{41.22556f, 9.94900f, -5.48997f}) < .001f);
    CHECK(glm::distance(native.attention(), Vec3{61.51f, -3.99f, -2.95f}) < .001f);
    CHECK(*fixture.level->camera() == native.camera());
    CHECK_FALSE(collision.pickSurface(native.attention(), native.camera().position));
    // Only the lower near-plane probe hits the ramp. It previously reduced
    // this clear 24.74-unit view to 8.18 units, producing the reported zoom.
    const Vec3 lowerProbe = -native.camera().up() * .5f;
    REQUIRE(collision.pickSurface(native.attention() + lowerProbe,
                                  native.camera().position + lowerProbe));
    CHECK(glm::distance(native.camera().position, native.attention()) > 24.7f);
    for (s32 tick = 0; tick < 90; ++tick) {
        party[0].feet.x += .025f;
        party[0].follow.x += .025f;
        native.update(party, 1.0f / 60);
        fixture.level->followCamera(party, 1.0f / 60);
        CHECK(*fixture.level->camera() == native.camera());
        for (const f32 alpha : {0.0f, .25f, .75f, 1.0f}) {
            CHECK(*fixture.level->camera(alpha) == native.sample(alpha / 60));
        }
    }
}

TEST_CASE("Only the Dreamcast tower adapter applies its native camera movement exclusion",
          "[legends][dreamcast-travel][dreamcast-camera][assets]") {
    for (const auto* const name : {"levelt1", "levelb1"}) {
        DYNAMIC_SECTION(name) {
            // NOLINTNEXTLINE(misc-const-correctness): the level changes behind MSVC's unique_ptr
            DreamcastTravelFixture fixture{name};
            fixture.level->resetCamera(std::array{CameraSubject{{0, 0, 0}, {0, 3, 0}, 3}});
            const auto camera = fixture.level->camera();
            REQUIRE(camera);
            const Vec3 from{camera->position.x + 12, 0, camera->position.z};
            const Vec3 to = from + Vec3{-1, 0, 1};
            const auto admitted = fixture.level->resolveMovement(from, to, .5f, 3);
            const bool tower = std::string_view{name} == "levelt1";
            CHECK(glm::distance(admitted, tower ? from + Vec3{0, 0, 1} : to) < .001f);
            CHECK(glm::distance(fixture.level->resolveMovement(from, from + Vec3{1, 0, 0}, .5f, 3),
                                from + Vec3{1, 0, 0}) < .001f);
        }
    }
}

TEST_CASE("Dreamcast travel resolves mixed-case extraction directories and its own Tower",
          "[legends][dreamcast-travel][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_regular_file(root / "levelb1/worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    const AssetLocator destination{root / "LeVeLb1"};
    REQUIRE(destination.find("WORLDS.ROM"));
    CHECK(std::filesystem::equivalent(destination.root(), root / "levelb1"));
    const auto tower = legendsTowerPath(LegendsPlatform::Dreamcast, destination.root());
    REQUIRE(tower);
    CHECK(std::filesystem::equivalent(*tower, root / "levelt1"));
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::N64, destination.root()));
}

} // namespace
