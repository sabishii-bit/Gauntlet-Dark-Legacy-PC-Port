#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Gates.h"
#include "formats/LegendsN64Scene.h"
#include "formats/LegendsN64Texture.h"
#include "formats/LegendsN64Tower.h"
#include "game/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "levels/tower2/worlds.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    return root;
}

formats::LegendsMap nativeMap(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    for (auto& texture : formats::decodeLegendsN64Textures(models)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

void capture(LegendsSceneAdapter& scene, test::FakeRenderDevice& device) {
    device.draws.clear();
    const auto camera = CameraFrame::of(WorldCamera{});
    scene.draw(device, Mat4{1}, camera, TreeModel::Pass::Opaque);
    scene.draw(device, Mat4{1}, camera, TreeModel::Pass::Blended);
}

std::vector<Vec3> nearDoor(const test::FakeRenderDevice& device, const Vec3& centre) {
    std::vector<Vec3> vertices;
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            const Vec3 delta = vertex.position - centre;
            if (glm::length(Vec2{delta.x, delta.z}) < 10) {
                vertices.push_back(vertex.position);
            }
        }
    }
    return vertices;
}

std::vector<std::vector<u8>> texturePixels(const std::filesystem::path& root,
                                           std::span<const formats::LegendsN64TowerRelic> relics) {
    std::vector<std::vector<u8>> result;
    for (const auto& relic : relics) {
        for (auto& texture :
             formats::decodeLegendsN64Textures(readFile(root / relic.archive / "objects.wad"))) {
            if (!texture.image.pixels.empty()) {
                result.push_back(std::move(texture.image.pixels));
            }
        }
    }
    REQUIRE_FALSE(result.empty());
    return result;
}

std::vector<ImmediateVertex> relicVertices(const test::FakeRenderDevice& device,
                                           const std::vector<std::vector<u8>>& images) {
    std::vector<ImmediateVertex> result;
    for (const auto& draw : device.draws) {
        const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
        if (texture != nullptr && std::ranges::find(images, texture->pixels) != images.end()) {
            result.insert(result.end(), draw.vertices.begin(), draw.vertices.end());
        }
    }
    return result;
}

TEST_CASE("Native Tower realm barriers use only the attending N64 obelisk union",
          "[legends][progression-scene][tower][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/tower2";
    auto map = nativeMap(directory);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    CharacterSave first;
    CharacterSave second;
    first.progress().relics.runes = 0x1fff;
    first.progress().relics.shards = 0xffff;
    first.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks = 0xff;
    const std::array<const CharacterSave*, 2> party{&first, &second};
    const Vec3 from{42, 2, -10};
    const Vec3 to{42, 2, -24};
    REQUIRE(scene->resolveMovement(from, to, 1, 6) != to);
    const auto allocated = device.texturesCreated;
    SECTION("Already earned union starts open") {
        first.progress().legendsProgress(LegendsPlatform::N64).obelisks = 3;
        second.progress().legendsProgress(LegendsPlatform::N64).obelisks = 4;
        scene->synchronizeProgress(party);
        REQUIRE(scene->resolveMovement(from, to, 1, 6) == to);
        capture(*scene, device);
        REQUIRE_FALSE(nearDoor(device, to).empty());
        CHECK(scene->takeSounds().empty());
    }
    SECTION("A new shared unlock releases the held opening animation once") {
        scene->synchronizeProgress(party);
        REQUIRE(scene->resolveMovement(from, to, 1, 6) != to);
        capture(*scene, device);
        const auto closed = nearDoor(device, to);
        REQUIRE_FALSE(closed.empty());
        first.progress().legendsProgress(LegendsPlatform::N64).obelisks = 3;
        scene->synchronizeProgress(party);
        REQUIRE(scene->resolveMovement(from, to, 1, 6) != to);
        second.progress().legendsProgress(LegendsPlatform::N64).obelisks = 4;
        scene->synchronizeProgress(party);
        REQUIRE(scene->resolveMovement(from, to, 1, 6) == to);
        const auto openingSounds = scene->takeSounds();
        REQUIRE(openingSounds.size() == 1);
        CHECK(openingSounds.front().token == 0x10);
        bool changed = false;
        for (s32 tick = 0; tick < 600; ++tick) {
            scene->capturePresentation();
            scene->update(1.0f / 60);
            scene->synchronizeProgress(party);
            if (tick % 30 == 29) {
                capture(*scene, device);
                changed |= nearDoor(device, to) != closed;
            }
        }
        CHECK(changed);
        CHECK(scene->takeSounds().empty());
        CHECK(scene->resolveMovement(from, to, 1, 6) == to);
        // Repeated synchronization never starts binding a new archive or figure.
        CHECK(device.texturesCreated == allocated);
    }
    const Vec3 battleFrom{0, -4, 30};
    const Vec3 battleTo{0, -4, 45};
    REQUIRE(scene->resolveMovement(battleFrom, battleTo, 1, 6) != battleTo);
}

TEST_CASE("Installed N64 Tower runes and shards draw from their native relic archives",
          "[legends][progression-scene][tower][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/tower2";
    auto map = nativeMap(directory);
    const auto config = formats::decodeLegendsN64Tower(readFile(root / "_program/Tower.bin"));
    const auto runeImages = texturePixels(root, config.runestones);
    const auto shardImages = texturePixels(root, config.shards);
    const auto beam = std::ranges::find(map.objects, std::string_view("CATH_BEAM"), &formats::LegendsMapObject::name);
    REQUIRE(beam != map.objects.end());
    const usize beamIndex = static_cast<usize>(beam - map.objects.begin());
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    CharacterSave save;
    const std::array<const CharacterSave*, 1> party{&save};
    save.progress().relics.runes = 0x1fff;
    save.progress().relics.shards = 0xffff;
    auto& unrelated = save.progress().legendsProgress(LegendsPlatform::Dreamcast);
    unrelated.runestones = 0x1fff;
    unrelated.shards = 0xf;
    scene->synchronizeProgress(party);
    capture(*scene, device);
    REQUIRE(relicVertices(device, runeImages).empty());
    REQUIRE(relicVertices(device, shardImages).empty());
    REQUIRE_FALSE(scene->worldObjectVisible(beamIndex));
    auto& progress = save.progress().legendsProgress(LegendsPlatform::N64);
    progress.runestones = 1;
    progress.shards = 1;
    scene->synchronizeProgress(party);
    capture(*scene, device);
    const auto oneRune = relicVertices(device, runeImages);
    const auto oneShard = relicVertices(device, shardImages);
    REQUIRE_FALSE(oneRune.empty());
    REQUIRE_FALSE(oneShard.empty());
    REQUIRE_FALSE(scene->worldObjectVisible(beamIndex));
    CHECK(std::ranges::all_of(oneShard, [](const auto& vertex) { return vertex.color.a <= 47; }));
    progress.runestones = 0x1fff;
    progress.shards = 0xf;
    scene->synchronizeProgress(party);
    capture(*scene, device);
    CHECK(relicVertices(device, runeImages).size() > oneRune.size());
    const auto allShards = relicVertices(device, shardImages);
    REQUIRE(allShards.size() > oneShard.size());
    CHECK(std::ranges::any_of(allShards, [](const auto& vertex) { return vertex.color.a == 255; }));
    CHECK(scene->worldObjectVisible(beamIndex));
}

TEST_CASE("Native obelisk grants are shared and a spent obelisk remains a visible solid body",
          "[legends][progression-scene][obelisks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/mount1";
    auto map = nativeMap(directory);
    const auto obeliskBytes = readFile(root / "items/c/obelisk/objects.wad");
    const auto obeliskModels = formats::decodeLegendsN64Models(obeliskBytes);
    const auto obeliskSequences = formats::decodeLegendsN64Sequences(
        readFile(root / "items/c/obelisk/seq.wad"), obeliskModels);
    const auto& trees = obeliskSequences.animation.trees;
    const auto ready = std::ranges::find(trees, std::string_view("TRAP:OBRD"), &formats::TreeDefinition::name);
    const auto spent = std::ranges::find(trees, std::string_view("TRAP:OBDN"), &formats::TreeDefinition::name);
    REQUIRE(ready != trees.end());
    REQUIRE(spent != trees.end());
    REQUIRE(ready->nodes.size() == 1);
    REQUIRE(spent->nodes.size() == 1);
    REQUIRE(ready->nodes.front().object == "WIZOBLSK_OFFNODE");
    REQUIRE(spent->nodes.front().object == "WIZOBLSK_ONNODE");
    // The native two states share their complete geometry. Their three material
    // lists select different artwork, so unchanged vertices are not a failed swap.
    REQUIRE(obeliskModels.size() == 2);
    REQUIRE(obeliskModels[0].mesh.vertices == obeliskModels[1].mesh.vertices);
    const auto images = formats::decodeLegendsN64Textures(obeliskBytes);
    const auto namedImages = [&](std::span<const std::string_view> names) {
        std::vector<std::vector<u8>> result;
        for (const auto name : names) {
            const auto found = std::ranges::find(images, name, &formats::LegendsN64Texture::name);
            REQUIRE(found != images.end());
            REQUIRE_FALSE(found->image.pixels.empty());
            result.push_back(found->image.pixels);
        }
        return result;
    };
    constexpr std::array<std::string_view, 3> kReadyNames{"WIZ_OBFRONT", "WIZ_OBSIDE", "WIZ_TOP"};
    constexpr std::array<std::string_view, 3> kSpentNames{"WIZ_OBFRONT2", "WIZ_OBSIDE2",
                                                          "WIZ_TOP2"};
    const auto readyImages = namedImages(kReadyNames);
    const auto spentImages = namedImages(kSpentNames);
    for (usize i = 0; i < readyImages.size(); ++i) {
        REQUIRE(readyImages[i] != spentImages[i]);
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    const auto pickups = scene->pickups();
    const auto found = std::ranges::find_if(pickups, [](const auto& pickup) {
        return pickup.award && pickup.award->kind == LegendsRelic::Obelisk;
    });
    REQUIRE(found != pickups.end());
    const usize index = static_cast<usize>(found - pickups.begin());
    const auto& pickup = *found;
    REQUIRE(pickup.award->index == 0);
    CharacterSave save;
    save.progress().health = 500;
    CharacterSave peer;
    peer.progress().health = 500;
    PlayerActor actor;
    actor.spawn(0, save, nullptr, pickup.position, 0);
    const std::array<CharacterSave*, 2> party{&actor.save(), &peer};
    const std::array<const CharacterSave*, 2> view{&actor.save(), &peer};
    scene->synchronizeProgress(view);
    const auto bodyAt = [&](const LegendsSceneAdapter& current) {
        const auto bodies = current.containerObstacles();
        const auto body = std::ranges::find_if(bodies, [&](const auto& candidate) {
            return candidate.solid && candidate.cylinderRadius == pickup.radius &&
                   glm::distance(candidate.centre, pickup.position) < .01f;
        });
        REQUIRE(body != bodies.end());
        REQUIRE(body->touchedBy(pickup.position, .75f));
    };
    bodyAt(*scene);
    capture(*scene, device);
    REQUIRE_FALSE(relicVertices(device, readyImages).empty());
    REQUIRE(relicVertices(device, spentImages).empty());
    scene->collect(actor, 1, false, {}, party);
    REQUIRE(scene->pickups()[index].collected);
    const auto obeliskSounds = scene->takeSounds();
    CHECK(std::ranges::count(obeliskSounds, 0x78U, &LegendsSoundEvent::token) == 1);
    CHECK(scene->takeSounds().empty());
    REQUIRE(actor.save()
                .progress()
                .legendsProgress(LegendsPlatform::N64)
                .has(LegendsRelic::Obelisk, 0));
    REQUIRE(peer.progress().legendsProgress(LegendsPlatform::N64).has(LegendsRelic::Obelisk, 0));
    REQUIRE(actor.save().progress().relics == save.progress().relics);
    REQUIRE(peer.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks == 0);
    scene->update(1.0f / 30);
    bodyAt(*scene);
    capture(*scene, device);
    CHECK(relicVertices(device, readyImages).empty());
    CHECK_FALSE(relicVertices(device, spentImages).empty());
    auto loaded = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    // Only the second player's ownership is enough on native level admission.
    actor.save().progress().legendsProgress(LegendsPlatform::N64).obelisks = 0;
    loaded->synchronizeProgress(view);
    REQUIRE(loaded->pickups()[index].collected);
    CHECK(loaded->takeSounds().empty());
    bodyAt(*loaded);
    capture(*loaded, device);
    CHECK(relicVertices(device, readyImages).empty());
    CHECK_FALSE(relicVertices(device, spentImages).empty());
    const auto after = peer;
    loaded->collect(actor, 1, false, {}, party);
    CHECK(peer == after);
    CHECK(actor.save().progress().legendsProgress(LegendsPlatform::N64).obelisks == 0);
}

TEST_CASE("A native level gate opens through the adapter by spending exactly one key",
          "[legends][progression-scene][gates][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/mount1";
    auto map = nativeMap(directory);
    const auto placements = readFile(directory / "ipr.rom");
    const auto traps = readFile(root / "items/generic/ptrap.rom");
    auto gates = formats::decodeLegendsN64Gates(placements, traps).gates;
    REQUIRE_FALSE(gates.empty());
    const auto chosen = std::ranges::find_if(gates, [](const auto& gate) {
        return gate.minimumPlayers <= 1 && (gate.placementFlags & 6U) == 0;
    });
    REQUIRE(chosen != gates.end());
    auto gate = *chosen;
    formats::LegendsN64Fixture fixture;
    fixture.kind = formats::LegendsN64FixtureKind::Trap;
    fixture.position = gate.position;
    fixture.groundProjected = gate.groundProjected;
    gate.position = formats::legendsN64FixturePosition(fixture, map.collision);
    const LegendsGate reference{gate};
    const auto primary = reference.primaryObstacle();
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto primaryPresent = [&] {
        const auto obstacles = scene->containerObstacles();
        return std::ranges::any_of(obstacles, [&](const auto& body) {
            return body.solid && glm::distance(body.centre, primary.centre) < .01f &&
                   body.halfAcross == primary.halfAcross && body.halfAlong == primary.halfAlong;
        });
    };
    REQUIRE(primaryPresent());
    CharacterSave save;
    save.progress().health = 500;
    PlayerActor actor;
    actor.spawn(0, save, nullptr, primary.centre, 0);
    scene->collect(actor, 1, false);
    REQUIRE(primaryPresent());
    CHECK(scene->takeSounds().empty());
    actor.save().progress().inventory.keys = 2;
    capture(*scene, device);
    const auto before = device.draws;
    scene->collect(actor, 1, false);
    REQUIRE(actor.save().progress().inventory.keys == 1);
    REQUIRE_FALSE(primaryPresent());
    const auto gateSounds = scene->takeSounds();
    REQUIRE(gate.sound != 0);
    CHECK(std::ranges::count(gateSounds, gate.sound, &LegendsSoundEvent::token) == 1);
    for (s32 tick = 0; tick < 120; ++tick) {
        scene->capturePresentation();
        scene->update(1.0f / 60);
        scene->collect(actor, 1, false);
    }
    CHECK(actor.save().progress().inventory.keys == 1);
    CHECK(scene->takeSounds().empty());
    CHECK_FALSE(primaryPresent());
    capture(*scene, device);
    bool changed = before.size() != device.draws.size();
    for (usize i = 0; i < std::min(before.size(), device.draws.size()); ++i) {
        changed |= before[i].vertices != device.draws[i].vertices;
    }
    CHECK(changed);
    // Native frame volumes are deliberately retained after the leaf opens.
    for (const auto& frame : reference.frameObstacles()) {
        if (!frame.solid) {
            continue;
        }
        const auto obstacles = scene->containerObstacles();
        CHECK(std::ranges::any_of(obstacles, [&](const auto& body) {
            return body.solid && glm::distance(body.centre, frame.centre) < .01f &&
                   body.halfAcross == frame.halfAcross && body.halfAlong == frame.halfAlong;
        }));
    }
}

TEST_CASE("Native Tower completion portals require the whole party and retain their waiting phase",
          "[legends][progression-scene][tower][portals][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/tower2";
    auto map = nativeMap(directory);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    CharacterSave save;
    const std::array<const CharacterSave*, 1> party{&save};
    Vec3 centre{0};
    std::string destination;
    SECTION("Thirteen N64 runestones open the Underworld route") {
        centre = {1.5f, 0, 14};
        destination = "hell1";
    }
    SECTION("Four N64 shards open the cathedral route") {
        centre = {0, 1, -30};
        destination = "boss1";
    }
    std::array<LegendsPortalVisitor, 2> visitors{{{0, centre}, {1, centre}}};
    save.progress().relics.runes = 0x1fff;
    save.progress().relics.shards = 0xffff;
    auto& dreamcast = save.progress().legendsProgress(LegendsPlatform::Dreamcast);
    dreamcast.runestones = 0x1fff;
    dreamcast.shards = 0xf;
    scene->synchronizeProgress(party);
    for (s32 tick = 0; tick < 300; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    auto& n64 = save.progress().legendsProgress(LegendsPlatform::N64);
    n64.runestones = 0x1fff;
    n64.shards = 0xf;
    scene->synchronizeProgress(party);
    visitors[1].position.x += 20;
    for (s32 tick = 0; tick < 300; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    visitors[1].position = centre;
    // Even an already charged portal retains its native all-party dwell.
    for (s32 tick = 0; tick < 60; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    std::optional<std::string> entered;
    for (s32 tick = 0; tick < 240 && !entered; ++tick) {
        entered = scene->tickPortals(visitors, 1.0f / 60);
    }
    REQUIRE(entered);
    CHECK(*entered == destination);
    for (s32 tick = 0; tick < 240; ++tick) {
        CHECK_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
}
TEST_CASE("Native Tower stage portals require preceding progress from the active N64 party",
          "[legends][progression-scene][unlocks][portal][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/tower2";
    auto map = nativeMap(directory);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    const auto portalIndex = [&](std::string_view name) {
        const auto portals = scene->portals();
        const auto found = std::ranges::find(portals, name, &formats::LegendsN64Portal::map);
        REQUIRE(found != portals.end());
        return static_cast<usize>(found - portals.begin());
    };
    const usize mount1 = portalIndex("mount1");
    const usize mount2 = portalIndex("mount2");
    const usize mount3 = portalIndex("mount3");
    CharacterSave first;
    CharacterSave second;
    auto& n64First = first.progress().legendsProgress(LegendsPlatform::N64);
    auto& n64Second = second.progress().legendsProgress(LegendsPlatform::N64);
    auto& dreamcast = first.progress().legendsProgress(LegendsPlatform::Dreamcast);
    REQUIRE(dreamcast.recordCompleted("mount1"));
    REQUIRE(dreamcast.recordCompleted("mount2"));
    dreamcast.obelisks = ~u64{0};
    first.progress().relics.runes = 0x1fff;
    first.progress().relics.shards = 0xffff;
    const std::array<const CharacterSave*, 2> party{&first, &second};
    const auto firstBefore = first;
    const auto secondBefore = second;
    CHECK_FALSE(scene->portalUnlocked(mount1));
    scene->synchronizeProgress(party);
    CHECK(scene->portalUnlocked(mount1));
    CHECK_FALSE(scene->portalUnlocked(mount2));
    CHECK_FALSE(scene->portalUnlocked(mount3));
    CHECK(first == firstBefore);
    CHECK(second == secondBefore);
    const Vec3 centre = scene->portals()[mount3].position;
    const std::array visitors{LegendsPortalVisitor{0, centre}, LegendsPortalVisitor{1, centre}};
    for (s32 tick = 0; tick < 300; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    capture(*scene, device);
    const auto lockedGeometry = nearDoor(device, scene->portals()[mount2].position);
    REQUIRE_FALSE(lockedGeometry.empty());
    REQUIRE(n64First.recordCompleted("mount1"));
    scene->synchronizeProgress(party);
    CHECK(scene->portalUnlocked(mount2));
    CHECK_FALSE(scene->portalUnlocked(mount3));
    capture(*scene, device);
    const auto unlockedGeometry = nearDoor(device, scene->portals()[mount2].position);
    CHECK(unlockedGeometry.size() > lockedGeometry.size());
    REQUIRE(n64Second.recordCompleted("mount2"));
    scene->synchronizeProgress(party);
    REQUIRE(scene->portalUnlocked(mount3));
    for (s32 tick = 0; tick < 100; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    // Removing the character that supplied the second completion revokes
    // access, even while both bodies are still standing on the charged exit.
    const std::array<const CharacterSave*, 1> reducedParty{&first};
    scene->synchronizeProgress(reducedParty);
    REQUIRE_FALSE(scene->portalUnlocked(mount3));
    for (s32 tick = 0; tick < 300; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    scene->synchronizeProgress(party);
    REQUIRE(scene->portalUnlocked(mount3));
    for (s32 tick = 0; tick < 120; ++tick) {
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
    }
    std::optional<std::string> destination;
    for (s32 tick = 0; tick < 180 && !destination; ++tick) {
        destination = scene->tickPortals(visitors, 1.0f / 60);
    }
    REQUIRE(destination);
    CHECK(*destination == "mount3");
}
} // namespace
