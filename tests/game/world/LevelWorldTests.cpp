#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/players/PlayerActor.h"
#include "game/players/Progression.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"
#include "game/world/TowerAccess.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("level pickups follow triggered floors from their initial poses",
          "[game][world][pickup-platform][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize supported = 0;
    usize moved = 0;
    for (const auto* name : {"E1", "G1"}) {
        CAPTURE(name);
        const auto level = catalog.byName(name);
        REQUIRE(level);
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root, *level));
        std::vector<std::pair<usize, Vec3>> riders;
        for (usize i = 0; i < world.placedItems().size(); ++i) {
            const auto& item = world.placedItems().item(i);
            if (!item.floor) {
                continue;
            }
            ++supported;
            riders.emplace_back(i, item.position);
            const auto object = static_cast<usize>(item.floor->object);
            CAPTURE(item.name, object);
            const auto& instance =
                world.layout().itemInstances()[static_cast<usize>(item.instance)];
            const Vec3 rest = world.layout().worldPosition(object);
            // The authored horizontal placement must survive in the floor's local frame.
            CHECK(item.floor->local[3].x == Approx(instance.position.x - rest.x).margin(0.001f));
            CHECK(item.floor->local[3].z == Approx(instance.position.z - rest.z).margin(0.001f));
            CHECK(glm::distance(item.position, Vec3{world.scene().worldTransform(object) *
                                                    item.floor->local[3]}) < 0.001f);
        }
        // The public trigger route must carry pickups even for instantaneous openings,
        // without requiring a later animation tick or a visible party.
        for (usize i = 0; i < world.triggers().size(); ++i) {
            world.activateTrigger(world.triggers().trigger(i).id, true);
        }
        for (const auto& [index, before] : riders) {
            const auto& item = world.placedItems().item(index);
            if (item.floor) {
                const auto object = static_cast<usize>(item.floor->object);
                CHECK(glm::distance(item.position, Vec3{world.scene().worldTransform(object) *
                                                        item.floor->local[3]}) < 0.001f);
                moved += glm::distance(before, item.position) > 0.1f ? 1 : 0;
            }
        }
    }
    CAPTURE(supported, moved);
    REQUIRE(supported > 0);
    REQUIRE(moved > 0);
}

TEST_CASE("Chimera approach lowers the chained arena elevator",
          "[game][world][chimera][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    constexpr usize kElevator = 25;
    REQUIRE(world.layout().objects()[kElevator].name == "A5ELEVATOR");
    const f32 initial = world.scene().worldTransform(kElevator)[3].y;
    std::array party{TriggerVisitor{.position = world.startPoint(0)->position}};
    bool walk = true;
    SECTION("walk from the retail entrance") {}
    SECTION("start inside the scenario's pressure region") {
        party[0].position = Vec3{0, 0, 10};
        walk = false;
    }
    const auto* marker = world.layout().findLocator(LocatorKind::Boss);
    REQUIRE(marker);
    Bosses boss;
    boss.open(device, root, &world.collision(), {}, 'A');
    REQUIRE(boss.spawn(35, marker->position, 0));
    const f32 initialBoss = boss.position()->y;
    world.startTriggers(party);
    bool halfway = false;
    for (s32 frame = 0; frame < 600; ++frame) {
        if (walk) {
            party[0].position.z = std::max(-10.0f, party[0].position.z - 0.2f);
        }
        if (const auto floor = world.collision().floorAt(party[0].position, 3, 10)) {
            party[0].position.y = floor->y;
            party[0].floorObject = floor->object;
        }
        world.updateTriggers(1.0f / 30, party);
        world.update(1.0f / 30);
        const std::array views{EnemyView{.position = party[0].position}};
        boss.update(2, 1.0f / 30, views);
        const f32 descent = initial - world.scene().worldTransform(kElevator)[3].y;
        if (!halfway && descent > 10 && descent < 20) {
            halfway = true;
            CHECK(boss.position()->y == Approx(initialBoss - descent).margin(0.15f));
        }
    }
    CAPTURE(party[0].position.y, party[0].position.z, initial);
    CHECK(world.triggers().opened(kElevator));
    CHECK(world.scene().worldTransform(kElevator)[3].y == Approx(initial - 23.0f));
    CHECK(halfway);
    CHECK(boss.position()->y == Approx(initialBoss - 23.0f).margin(0.15f));
}

TEST_CASE("Temple spawning and entrance movement reject the wall-only underlay",
          "[game][world][collision][temple][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    for (const auto* kind : {"ICE", "IMP", "PLA", "ZOM"}) {
        test::unpackedOrSkip(std::string("MONSTERS/") + kind + "/animations.json");
    }
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto& collision = world.collision();
    // Seed 1 previously put an ICE from a front-room generator at this location,
    // five units below the player, on E1#32 rather than a walkable floor.
    const Vec3 underground{-19.283f, 0, 66.033f};
    CHECK_FALSE(collision.floorAt(underground, 6, 6));
    CHECK_FALSE(collision.floorAt({0, 0, 110}, 6, 6));
    Enemies enemies;
    enemies.open(device, root, &collision, Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(
        generators.bind(device, world.layout(), enemies, &collision, {}, 1, {}, 5, &world.items()));
    REQUIRE(generators.count() > 0);
    REQUIRE_FALSE(
        enemies.spawn(EnemySpawn{.kind = 16, .position = underground, .placed = true}, {}));
    for (s32 g = 0; g < static_cast<s32>(generators.count()); ++g) {
        const std::array party{EnemyView{.position = generators.positionOf(g) + Vec3{0, 0, 8}}};
        generators.update(600, enemies, party);
        for (const auto& enemy : enemies.targets()) {
            CAPTURE(g, enemy.id, enemy.base.x, enemy.base.y, enemy.base.z);
            CHECK(enemy.base.y >= 0);
            const auto floor = collision.floorAt(enemy.base, 0.01f, 0.01f);
            REQUIRE(floor);
            CHECK((floor->objectFlags & WorldCollision::kFloorQueryFlags) != 0);
        }
    }
    REQUIRE(enemies.count() > 0);
    const auto* start = world.startPoint(0);
    REQUIRE(start);
    // Walk outwards and to either side of the entrance: no route may reach the
    // non-walkable apron at y=-5 (the old implementation reached z=130).
    for (const Vec2 direction : {Vec2{0, 1}, Vec2{1, 0}, Vec2{-1, 0}, Vec2{1, 1}, Vec2{-1, 1}}) {
        PlayerActor actor;
        actor.spawn(0, {}, nullptr, start->position, 0);
        for (s32 frame = 0; frame < 1200; ++frame) {
            actor.update({glm::normalize(direction), 1}, 0, 1.0f / 60, &collision);
        }
        CAPTURE(direction.x, direction.y, actor.position().x, actor.position().z);
        CHECK(actor.position().y >= -4);
        // Feet may straddle the boundary by the small floor-contact margin, but
        // cannot reach the apron or keep walking past the last supporting face.
        CHECK(actor.position().z <= 105 + PlayerActor::kFloorEdgeReach);
        CHECK(collision.floorAt(actor.position(), 0.01f, 0.01f, PlayerActor::kFloorEdgeReach));
        CHECK(std::abs(actor.position().x) <= 20.001f);
    }
}

TEST_CASE("unanimated Desert bridges appear on contact and vanish after release",
          "[game][world][triggers][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA3/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A3");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    constexpr usize kBarrier = 170;
    const auto& objects = world.layout().objects();
    REQUIRE((objects[kBarrier].flags & WorldObject::kAnimated) == 0);
    const auto& pad = world.layout().itemInstances()[16];
    world.startTriggers({});
    CHECK(world.scene().objectAlpha(kBarrier) == 0.0f);
    const std::array party{TriggerVisitor{.position = pad.position}};
    world.updateTriggers(1.0f, party);
    CHECK(world.scene().objectAlpha(kBarrier) == 1.0f);
    CHECK(world.collision().solid(static_cast<s32>(kBarrier)));
    world.updateTriggers(1.0f, {});
    CHECK(world.scene().objectAlpha(kBarrier) == 0.0f);
    CHECK_FALSE(world.collision().solid(static_cast<s32>(kBarrier)));
}

TEST_CASE("the tower loads its geometry, collision, start points and camera markers",
          "[game][world][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("LEVELS/LEVELL1/collision.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE_FALSE(tower.built());
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.built());
    REQUIRE(tower.scene().placedCount() > 2000);
    REQUIRE(tower.collision().loaded());
    REQUIRE(tower.cameraMarkers().size() == 79); // the game camera markers
    REQUIRE(tower.entranceCamera().has_value());

    const WorldLocator* entrance = tower.startPoint(0);
    REQUIRE(entrance != nullptr);
    REQUIRE(entrance->kind == LocatorKind::Start);
    REQUIRE(tower.collision().floorAt(entrance->position, 3.0f, 3.0f).has_value());
    REQUIRE(tower.startPoint(99) == nullptr);
    // Back from the town realm (7) the party stands among its portals, which is the tower's
    // second start marker; from a realm with no ring of its own, at the entrance.
    REQUIRE(LevelWorld::towerMarkerOf(7) == 1);
    REQUIRE(LevelWorld::towerMarkerOf(2) == 2);
    REQUIRE(LevelWorld::towerMarkerOf(13) == 0);
    REQUIRE(LevelWorld::towerMarkerOf(99) == 0);
    REQUIRE(tower.arrivalPoint(7) == tower.startPoint(1));
    REQUIRE(glm::distance(tower.arrivalPoint(7)->position, Vec3{37.8f, -6.3f, -117.5f}) < 0.5f);
    REQUIRE(tower.arrivalPoint(13) == entrance);

    tower.clear();
    REQUIRE_FALSE(tower.built());
    REQUIRE(tower.cameraMarkers().empty());
}

TEST_CASE("the tower takes its light, camera range and sounds from the realm's data",
          "[game][world][unpacked]") {
    test::unpackedOrSkip("LEVELS/LEVELL1/world.json");
    const std::filesystem::path root =
        test::unpackedOrSkip("wdata/TOWER.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.hasLevelData());
    REQUIRE(tower.lighting().ambient.x == Approx(0.8f));
    const Vec3 expected = glm::normalize(Vec3{1.0f, 6.0f, -2.0f});
    REQUIRE(glm::all(glm::epsilonEqual(tower.lighting().direction, expected, 1e-5f)));
    REQUIRE(tower.cameraRange().radiusMin == 24.0f);
    REQUIRE(tower.cameraRange().radiusMax == 32.0f);
    REQUIRE(tower.cameraRange().minPitch == Approx(0.6981317f));
    REQUIRE(tower.audio() != nullptr);
    REQUIRE(tower.audio()->bank == "WIZTOWER");
    REQUIRE(tower.audio()->stream == "tower");
    REQUIRE(tower.wallHitSound() == "S_WEAPONHITSTON");
    // The lit level draws its lightmapped batches with their lightmaps.
    tower.draw(device, Mat4{1.0f}, WorldCamera{});
    usize lightmapped = 0;
    for (const auto& draw : device.draws) {
        lightmapped += draw.lightmap() != nullptr ? 1 : 0;
    }
    REQUIRE(lightmapped > 100);
}

TEST_CASE("returning parties do not respawn the province's introductory tower crystals",
          "[tower-crystals][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    tower.setPlayerCount(2);
    std::array<TriggerVisitor, 2> party;
    party[1].crystals[1] = 14;
    tower.startTriggers(party);
    const auto visible = [&] {
        usize count = 0;
        const auto& items = tower.placedItems();
        for (usize i = 0; i < items.size(); ++i) {
            count += items.item(i).subtype == ItemInfo::kCrystal && items.item(i).visible ? 1 : 0;
        }
        return count;
    };
    REQUIRE(visible() == 15);
    party[1].crystals[1] = 15; // Any member qualifies, including one joining a newcomer.
    tower.startTriggers(party);
    CHECK(visible() == 0);
    tower.setPlayerCount(1);
    tower.hideCrystals();
    tower.revealCrystals(100);
    CHECK(visible() == 0);
    REQUIRE(tower.load(device, root));
    tower.setPlayerCount(2);
    tower.startTriggers(party);
    CHECK(visible() == 0);
}

TEST_CASE("the tower moves its objects, flickers its torches and lends Sumner his archive",
          "[game][world][unpacked]") {
    test::unpackedOrSkip("LEVELS/LEVELL1/animations.json");
    const std::filesystem::path root = test::unpackedOrSkip("ITEMS/LEVELL/textures.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.hasItems());
    REQUIRE(tower.items().trees.find("GWIZ").has_value());
    REQUIRE(tower.worldAnimator().size() == 170);
    REQUIRE(tower.textureAnimator().size() >= 50);
    REQUIRE(tower.scene().unitCount() > 500);
    // The torch flames are the item archive's, not white, and change every second frame.
    constexpr u32 kTorchSlot = 423;
    const Texture* torch = tower.scene().textureOf(kTorchSlot);
    REQUIRE(torch != nullptr);
    REQUIRE(torch != &device.whiteTexture());
    tower.update(2.0f / 30.0f);
    REQUIRE(tower.scene().textureOf(kTorchSlot) != torch);
    // The crossing bridge waits for its trigger; the goldfish swim on.
    REQUIRE(tower.worldAnimator().held(0));
    REQUIRE(tower.worldAnimator().frame(0) == 0.0f);
    usize swimming = 0;
    for (usize i = 0; i < tower.worldAnimator().size(); ++i) {
        if (!tower.worldAnimator().held(i) && tower.worldAnimator().frame(i) == Approx(2.0f)) {
            ++swimming;
        }
    }
    REQUIRE(swimming > 50);
    REQUIRE(tower.triggers().size() == 99);
    REQUIRE(tower.collision().movingObjectCount() > 100);
    // The first realm's force field stands across its gate, solid, its own two triangles
    // placed where the level puts the object.
    std::optional<usize> field;
    for (usize i = 0; i < tower.layout().objects().size(); ++i) {
        if (tower.layout().objects()[i].name == "L1XPTRAPWG") {
            field = i;
        }
    }
    REQUIRE(field.has_value());
    REQUIRE(tower.collision().moving(static_cast<s32>(*field)));
    REQUIRE(tower.collision().solid(static_cast<s32>(*field)));
    const Vec3 gate = tower.layout().worldPosition(*field);
    REQUIRE(tower.collision().resolveWalls(gate, 0.75f, gate.y - 1.0f, gate.y + 1.0f) != gate);
    // Every particle marker names one of the level's templates; the braziers burn with the
    // item archive's flame.
    REQUIRE(tower.particles().size() == 206);
    REQUIRE(tower.particles().particleCount() > 100);
    // The crystals Sumner keeps: fifteen orange gems on the floor, for a party of one.
    REQUIRE(tower.placedItems().size() >= 15);
    REQUIRE(tower.placedItems().visibleCount() == 0);
    tower.setPlayerCount(1);
    usize gems = 0;
    for (usize i = 0; i < tower.placedItems().size(); ++i) {
        const PlacedItems::Item& item = tower.placedItems().item(i);
        if (item.name == "GEMORANGE") {
            ++gems;
            REQUIRE(item.visible);
            REQUIRE(item.subtype == ItemInfo::kCrystal);
            REQUIRE(item.instance >= 0);
        }
    }
    REQUIRE(gems == 15);
    REQUIRE(tower.placedItems().visibleCount() >= 15);
    const auto torchTexture = tower.items().textures.find("P_TORCH");
    REQUIRE(torchTexture.has_value());
    bool flames = false;
    for (usize i = 0; i < tower.particles().size(); ++i) {
        flames = flames || tower.particles().textureOf(i) ==
                               &tower.items().textures.texture(device, *torchTexture);
    }
    REQUIRE(flames);
    WorldCamera camera;
    camera.position = Vec3{0.0f, 20.0f, 0.0f};
    constexpr u32 kFieldSlot = 141;
    const Texture* fieldTexture = tower.scene().textureOf(kFieldSlot);
    REQUIRE(fieldTexture != nullptr);
    tower.scene().drawOpaque(device, Mat4{1}, CameraFrame::at(camera.position));
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != fieldTexture); // A backdrop would be overwritten by tower solids.
    }
    device.draws.clear();
    tower.scene().drawDeferred(device, Mat4{1}, CameraFrame::at(camera.position));
    usize closedFields = 0;
    for (const auto& draw : device.draws) {
        if (draw.texture == fieldTexture) {
            ++closedFields;
            CHECK(draw.blend() == BlendMode::Additive);
            CHECK_FALSE(draw.state.depthWrite);
            CHECK(draw.vertices.front().color.a == 255);
        }
    }
    CHECK(closedFields == 8);
    device.draws.clear();
    tower.draw(device, Mat4{1.0f}, camera);
    REQUIRE(device.draws.size() > 1000);
    // The flames are drawn added onto the frame, after the geometry.
    const Texture* flame = &tower.items().textures.texture(device, *torchTexture);
    bool flamesDrawn = false;
    for (const auto& draw : device.draws) {
        flamesDrawn =
            flamesDrawn || (draw.texture == flame && draw.state.blend == BlendMode::Additive);
    }
    REQUIRE(flamesDrawn);
    // The force field across the first realm's gate: its two triangles added onto the frame
    // with the field texture (animated, so whichever frame the slot shows), at full
    // brightness whichever way it faces.
    bool fieldDrawn = false;
    for (const auto& draw : device.draws) {
        if (draw.texture != fieldTexture || draw.state.blend != BlendMode::Additive ||
            draw.vertices.size() != 6) {
            continue;
        }
        fieldDrawn = true;
        REQUIRE(draw.vertices[0].color.r == 255);
        REQUIRE(draw.vertices[0].color.g == 255);
        REQUIRE(draw.vertices[0].color.b == 255);
    }
    REQUIRE(fieldDrawn);
    tower.clear();
    REQUIRE_FALSE(tower.hasItems());
    REQUIRE(tower.worldAnimator().size() == 0);
    REQUIRE(tower.particles().size() == 0);
    REQUIRE(tower.placedItems().size() == 0);
}

TEST_CASE("the tower reports a missing level without building", "[game][world]") {
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE_FALSE(tower.load(device, test::scratchDirectory("tower-world-none")));
    REQUIRE_FALSE(tower.built());
    REQUIRE_FALSE(tower.hasLevelData());
    REQUIRE_FALSE(tower.entranceCamera().has_value());
}

TEST_CASE("boss arenas can borrow torch particles from the realm beside their own items",
          "[game][world][boss-arena][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELD5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELD5/animations.json");
    test::unpackedOrSkip("ITEMS/LEVELD/animations.json");
    test::unpackedOrSkip("wdata/FOREST.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("D5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.hasItems());
    REQUIRE(world.realmItems().loaded());
    REQUIRE_FALSE(world.items().textures.find("P_TORCH").has_value());
    const auto slot = world.realmItems().textures.find("P_TORCH");
    REQUIRE(slot.has_value());
    const auto* torch = &world.realmItems().textures.texture(device, *slot);
    REQUIRE(torch != &device.whiteTexture());
    usize torches = 0;
    for (usize i = 0; i < world.particles().size(); ++i) {
        torches += world.particles().textureOf(i) == torch ? 1 : 0;
    }
    REQUIRE(torches > 0);
    world.clear();
    REQUIRE(world.particles().size() == 0);
    REQUIRE_FALSE(world.realmItems().loaded());
}

TEST_CASE("the tower puts out a shut portal's glow, opens the lifts and stands a party back "
          "from a shut world at the entrance",
          "[game][world][tower-access][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    const auto objectNamed = [&](std::string_view name) {
        const auto& objects = tower.layout().objects();
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == name) {
                return i;
            }
        }
        FAIL("no object " << name);
        return usize{0};
    };
    // Each glow is a unit of its own that the tower can hide, leaving its neighbours lit.
    REQUIRE(tower.shutPortalGlow(7, 2));
    REQUIRE_FALSE(tower.scene().objectVisible(objectNamed("L1NSNCG3_ACTIVE")));
    REQUIRE(tower.scene().objectVisible(objectNamed("L1NSNCG2_ACTIVE")));
    REQUIRE(tower.scene().objectVisible(objectNamed("L1NSNCG4_ACTIVE")));
    REQUIRE_FALSE(tower.shutPortalGlow(5, 0)); // the temple's portal has no glow object
    // A party back from the town stands among its portals only while the town is open to it.
    std::vector<ClassProgress> party(1);
    const TowerAccess shut{party};
    REQUIRE(tower.startPoint(1) != nullptr);
    REQUIRE(tower.arrivalPoint(7, &shut) == tower.startPoint(0));
    party[0].crystals[1] = 15;
    const TowerAccess open{party};
    REQUIRE(tower.arrivalPoint(7, &open) == tower.startPoint(1));
    REQUIRE(tower.arrivalPoint(7) == tower.startPoint(1));
    // The lifts down to the battlefield's portals stand open once its first level is beaten.
    const usize lift = objectNamed("L1LIFT01");
    const usize elevator = objectNamed("L1ELEV02");
    tower.setPlayerCount(1);
    std::array<TriggerVisitor, 1> visitors;
    visitors[0].crystals = party[0].crystals;
    tower.startTriggers(visitors, &open);
    REQUIRE_FALSE(tower.triggers().opened(static_cast<s32>(lift)));
    REQUIRE_FALSE(tower.triggers().opened(static_cast<s32>(elevator)));
    party[0].levels.recordBeaten(8, 0);
    const TowerAccess lifts{party};
    tower.startTriggers(visitors, &lifts);
    REQUIRE(tower.triggers().opened(static_cast<s32>(lift)));
    REQUIRE(tower.triggers().opened(static_cast<s32>(elevator)));
}

} // namespace
