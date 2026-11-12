#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

class ImportedPresentation final : public ImportedLevel {
public:
    s32 captures = 0;
    std::vector<std::pair<f32, bool>> updates;
    mutable std::vector<TreeModel::Pass> passes;
    mutable std::vector<f32> alphas;
    std::vector<u32> hazardFlags;
    std::vector<std::optional<Vec3>> bossAnchors;

    void capturePresentation() override { ++captures; }
    void update(f32 seconds, bool stopped) override { updates.emplace_back(seconds, stopped); }
    void trackBoss(std::optional<Vec3> anchor) override { bossAnchors.push_back(anchor); }
    std::span<const u32> surfaceHazardFlags() const override { return hazardFlags; }
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass pass, f32 alpha) const override {
        passes.push_back(pass);
        alphas.push_back(alpha);
    }
};

TEST_CASE("Imported floor-only boss tracking waits for the encounter to begin",
          "[legends][imported-level][boss-camera]") {
    ImportedPresentation presentation;
    ImportedLevel& level = presentation;
    BossCameraSubject boss;
    boss.position = {3, -20, 5};
    level.trackBoss(boss);
    CHECK(presentation.bossAnchors.empty());

    boss.engaged = true;
    boss.position.y = 0;
    level.trackBoss(boss);
    REQUIRE(presentation.bossAnchors.size() == 1);
    CHECK(presentation.bossAnchors.back() == std::optional<Vec3>{boss.position});
    level.trackBoss(std::nullopt);
    REQUIRE(presentation.bossAnchors.size() == 2);
    CHECK_FALSE(presentation.bossAnchors.back());
}

TEST_CASE("Imported levels' legend rites restrain the party as the base game's unless they say "
          "otherwise",
          "[legends][imported-level][legend]") {
    const ImportedPresentation presentation;
    const ImportedLevel& level = presentation;
    CHECK(level.legendRiteRestrainsParty());
}

WorldCollision importedCollision() {
    WorldCollision collision;
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {{{-20, 0, -20}, {0, 0, 20}, {20, 0, -20}}};
    // Native collision owner IDs do not require invented common world meshes.
    floor.object = 71;
    floor.objectFlags = 4;
    collision.build({floor});
    return collision;
}

WorldLayout importedLayout() {
    return WorldLayout::fromDecoded({},
                                    {{.kind = LocatorKind::Start, .position = {0, 0, 0}},
                                     {.kind = LocatorKind::CameraGame, .position = {0, 20, 30}}},
                                    {-20, 0, -20}, {20, 30, 20});
}

LevelRef importedRef() {
    LevelRef level;
    level.realm = "Legends";
    level.realmId = -1;
    level.name = "n64:tower2";
    level.title = "Tower";
    return level;
}

TEST_CASE("Imported levels enter the ordinary world pipeline with isolated common metadata",
          "[legends][imported-level]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    auto imported = std::make_shared<ImportedPresentation>();
    REQUIRE(world.loadImported(device, {}, importedRef(), importedLayout(), importedCollision(),
                               imported));
    REQUIRE(world.built());
    CHECK_FALSE(world.isTower());
    CHECK(world.imported() == imported.get());
    CHECK(std::as_const(world).imported() == imported.get());
    CHECK(world.ref().realmId == -1);
    CHECK(world.level() == nullptr);
    CHECK(world.audio() == nullptr);
    CHECK_FALSE(world.hasLevelData());
    CHECK_FALSE(world.layout().loaded());
    CHECK(world.layout().objects().empty());
    CHECK(world.layout().itemInfos().empty());
    CHECK(world.layout().itemInstances().empty());
    CHECK(world.placedItems().size() == 0);
    CHECK(world.triggers().size() == 0);
    CHECK(world.collision().triangleCount() == 1);
    const auto floor = world.collision().floorAt({0, 1, 0}, 2, 2);
    REQUIRE(floor);
    CHECK(floor->object == 71);
    REQUIRE(world.startPoint(0));
    CHECK(world.startPoint(0)->position == Vec3{0});
    CHECK(world.cameraMarkers().size() == 1);
    CHECK(world.cameraRange().boundsMin == Vec3{-20, 0, -20});
    CHECK_FALSE(imported->camera());
    CHECK(imported->resolveMovement(Vec3{0}, {1, 2, 3}, 1, 5) == Vec3{1, 2, 3});

    world.capturePresentation();
    world.update(1.0f / 60, true);
    world.setPlayerCount(4);
    world.startTriggers({});
    world.updateTriggers(1.0f / 60, {});
    world.draw(device, Mat4{1}, {}, .25f);
    REQUIRE(imported->updates.size() == 1);
    CHECK(imported->updates[0].first == 1.0f / 60);
    CHECK(imported->updates[0].second);
    CHECK(imported->captures == 1);
    CHECK(imported->passes == std::vector{TreeModel::Pass::Opaque, TreeModel::Pass::Blended});
    CHECK(imported->alphas == std::vector<f32>{.25f, .25f});

    // The presentation callback has no rewind schema. It must not accidentally
    // authenticate an incomplete network state through the common scene API.
    NativeStateDigest digest;
    CHECK_FALSE(world.appendNativeDigest(digest));
    CHECK_FALSE(digest.good());
    LevelWorld::LocalSnapshot saved;
    world.saveLocal(saved);
    CHECK_FALSE(world.canRestoreLocal(saved));
    CHECK_FALSE(world.restoreLocal(saved));

    const std::weak_ptr<ImportedPresentation> lifetime = imported;
    imported.reset();
    CHECK_FALSE(lifetime.expired());
    world.clear();
    CHECK(lifetime.expired());
    CHECK_FALSE(world.built());
    CHECK(world.imported() == nullptr);
    CHECK_FALSE(world.collision().loaded());
}

TEST_CASE("Rejected imported loads do not replace a working imported world",
          "[legends][imported-level]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    auto imported = std::make_shared<ImportedPresentation>();
    REQUIRE(world.loadImported(device, {}, importedRef(), importedLayout(), importedCollision(),
                               imported));
    CHECK_FALSE(world.loadImported(device, {}, LevelRef::tower(), importedLayout(),
                                   importedCollision(), std::make_shared<ImportedPresentation>()));
    CHECK_FALSE(world.loadImported(device, {}, importedRef(), importedLayout(), WorldCollision{},
                                   std::make_shared<ImportedPresentation>()));
    CHECK_FALSE(world.loadImported(device, {}, importedRef(), importedLayout(), importedCollision(),
                                   nullptr));
    CHECK(world.imported() == imported.get());
    CHECK(world.built());
    CHECK(world.collision().triangleCount() == 1);
}

TEST_CASE("Imported harmful surfaces use collision owners without invented world meshes",
          "[legends][imported-level][hazards]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    auto imported = std::make_shared<ImportedPresentation>();
    imported->hazardFlags.resize(72);
    imported->hazardFlags[71] = HazardSurfaces::kBurning;
    REQUIRE(world.loadImported(device, {}, importedRef(), importedLayout(), importedCollision(),
                               imported));
    CHECK(world.layout().objects().empty());
    CHECK(world.hazards().harmful() == 1);
    const auto contact = world.hazards().touching(world.collision(), {0, 0, 0}, 1, 5);
    REQUIRE(contact);
    CHECK(contact->object == 71);
    CHECK(contact->harm.damage == 5);
    REQUIRE(HazardSurfaces::enemyHarmOf(world.hazards().flagsOf(71)));
    CHECK_FALSE(world.hazards().harmOfObject(70));
    // Reopening an importer without hazard metadata must clear the old roster.
    REQUIRE(world.loadImported(device, {}, importedRef(), importedLayout(), importedCollision(),
                               std::make_shared<ImportedPresentation>()));
    CHECK(world.hazards().harmful() == 0);
    CHECK_FALSE(world.hazards().touching(world.collision(), {0, 0, 0}, 1, 5));
}

TEST_CASE("Imported levels keep common character powerup assets available",
          "[legends][imported-level][assets]") {
    const auto root = test::assetOrSkip("POWERUPS").parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.loadImported(device, root, importedRef(), importedLayout(), importedCollision(),
                               std::make_shared<ImportedPresentation>()));
    CHECK(world.powerups().loaded());
    CHECK(world.placedItems().size() == 0);
    CHECK(world.layout().itemInfos().empty());
}
} // namespace
