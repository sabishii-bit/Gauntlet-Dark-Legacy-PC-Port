#include <array>
#include <filesystem>
#include <format>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/Party.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/Progression.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The original game tree, without any exporter-produced fixtures. */
std::filesystem::path nativeRoot() {
    return test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
}

TEST_CASE("native tower gameplay loads player tuning and responds to movement and attacks",
          "[game][screens][native-gameplay][assets]") {
    const auto root = nativeRoot();
    const AssetLocator assets(root);
    REQUIRE(assets.find("LEVELS/LEVELL1/WORLDS.PS2"));
    REQUIRE_FALSE(assets.find("pdata/WAR.json"));
    REQUIRE_FALSE(assets.find("PLAYERS/WAR/YEL00/objects.json"));
    ClassDataSet classes;
    REQUIRE(classes.load(root / "pdata"));
    const auto* stats = classes.stats(0);
    REQUIRE(stats != nullptr);
    REQUIRE_FALSE(stats->moveStrikes.empty());
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto tower = catalog.byName("L1");
    REQUIRE(tower);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *tower));
    REQUIRE(world.level() != nullptr);
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.tower = &world;
    context.levels = &catalog;
    context.unpackedRoot = root;
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayOptions options;
    options.position = Vec3{3.0f, 2.0f, -20.0f}; // the tower-turbo scenario's clear floor
    options.yaw = 0.0f;
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.actor(0) != nullptr);
    REQUIRE(scene.animator(0) != nullptr);
    REQUIRE(scene.animator(0)->bound());
    REQUIRE(scene.weaponHeld(0));
    REQUIRE(scene.figureDirectory(0));
    CHECK(scene.figureDirectory(0)->filename() == "YEL00");
    CHECK(scene.actor(0)->height() == stats->height);
    CHECK(scene.actor(0)->radius() == stats->width * 0.5f);
    const auto expectedStats = displayStats(*stats, 1, party.front().save.progress());
    const f32 expectedSpeed =
        PlayerActor::kMinSpeed + static_cast<f32>(expectedStats.speed()) * PlayerActor::kStatScale *
                                     (PlayerActor::kMaxSpeed - PlayerActor::kMinSpeed);
    CHECK(scene.actor(0)->speed() == Catch::Approx(expectedSpeed));
    for (s32 tick = 0; tick < 180; ++tick) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.animator(0)->entering());
    const Vec3 before = scene.actor(0)->position();
    PlayScene::Inputs movement{};
    movement[0].move = {Vec2{1.0f, 0.0f}, 1.0f};
    for (s32 tick = 0; tick < 15; ++tick) {
        REQUIRE(scene.update(1.0 / 30, movement) == PlayOutcome::Running);
    }
    CHECK(glm::distance(scene.actor(0)->position(), before) > 1.0f);
    CHECK(scene.actor(0)->moving());
    for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
        PlayScene::Inputs attack{};
        attack[0].attack = true;
        attack[0].attackPressed = tick == 0;
        REQUIRE(scene.update(1.0 / 30, attack) == PlayOutcome::Running);
    }
    REQUIRE(scene.missiles().count() > 0);
    CHECK(scene.missiles().missile(0).owner == 0);
    CHECK(scene.missiles().missile(0).damage > 0);
    device.draws.clear();
    scene.missiles().draw(device, Mat4{1}, world.lighting());
    REQUIRE_FALSE(device.draws.empty());
    device.draws.clear();
    scene.render(device, makeScreenProjection(512, 384), 512, 384);
    REQUIRE_FALSE(device.draws.empty());
    scene.close(); // release figures/effect borrowers before world and device
    CHECK(scene.missiles().count() == 0);
    CHECK(scene.actorCount() == 0);
}

TEST_CASE("native costume tiers bind their own models and shared class animation",
          "[game][figure][native-gameplay][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    for (const s32 level : {1, 10, 50}) {
        CAPTURE(level);
        CharacterSave save;
        save.progress().experience = levelExperience(level);
        save.progress().promotedLevel = level;
        const auto directory = PlayerFigure::costumeDirectory(root, save);
        CHECK(directory.filename() == std::format("YEL{}0", level / 10));
        REQUIRE(AssetLocator(directory).find("objects.ngc"));
        auto figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        REQUIRE(figure->heldWeaponBound());
        REQUIRE(figure->animator().bound());
        REQUIRE(figure->missile().bound());
        figure->animate(1.0f, 2, 1.0f / 30);
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1.0f, false);
        CHECK_FALSE(device.draws.empty());
    }
}

TEST_CASE("native shop screen opens with retail catalog tuning fonts and artwork",
          "[game][screens][shop][native-gameplay][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    const std::array party{PartyMember{0, CharacterSave{}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "L1", ShopVisit::Shop));
    REQUIRE(scene.isOpen());
    REQUIRE_FALSE(scene.session().catalog().items().empty());
    REQUIRE(scene.session().lanes().size() == 1);
    CHECK(scene.session().lanes().front().stats.fightMin == 600.0f);
    scene.update(1.0 / 30, {});
    device.draws.clear();
    scene.render(device, makeScreenProjection(512, 384), 512, 384);
    REQUIRE_FALSE(device.draws.empty());
    scene.close();
    CHECK_FALSE(scene.isOpen());
}

TEST_CASE("every available native catalog stage loads through the complete world runtime",
          "[game][world][native-gameplay][native-level-census][assets]") {
    const auto root = nativeRoot();
    const AssetLocator assets(root);
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::vector<LevelRef> available;
    usize catalogCount = 0;
    for (const auto& realm : catalog.realms()) {
        CAPTURE(realm.file);
        REQUIRE(assets.find("WDATA/" + realm.file + ".WAD"));
        REQUIRE_FALSE(assets.find("WDATA/" + realm.file + ".json"));
        REQUIRE(catalog.levelCount(realm.id) == realm.levels.size());
        catalogCount += realm.levels.size();
        for (const auto& name : realm.levels) {
            CAPTURE(name);
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            REQUIRE(ref->realmId == realm.id);
            // Some catalogs can describe stages not installed with a partial asset tree.
            // The independent native-file inventory, not load success, selects this roster.
            if (assets.find(ref->directory + "/WORLDS.PS2")) {
                available.push_back(*ref);
            }
        }
    }
    CAPTURE(catalogCount, available.size());
    REQUIRE(catalogCount > 0);
    REQUIRE_FALSE(available.empty());
    usize loaded = 0;
    for (const auto& ref : available) {
        CAPTURE(ref.realm, ref.name, ref.directory);
        const AssetLocator files(root / ref.directory);
        for (const auto* manifest :
             {"world.json", "collision.json", "objects.json", "textures.json", "animations.json"}) {
            CAPTURE(manifest);
            REQUIRE_FALSE(files.find(manifest));
        }
        REQUIRE(LevelCatalog::unpacked(root, ref));
        // Scope the renderer and world to one stage: captured draws borrow its textures,
        // and the runtime's item/effect borrowers must retire before their archives.
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root, ref));
        REQUIRE(world.built());
        CHECK(world.ref() == ref);
        REQUIRE(world.level() != nullptr);
        CHECK(world.level()->name == ref.name);
        REQUIRE_FALSE(world.layout().objects().empty());
        CHECK(world.scene().placedCount() > 0);
        CHECK(world.scene().triangleCount() > 0);
        REQUIRE(world.collision().loaded());
        CHECK(world.collision().triangleCount() > 0);
        REQUIRE(world.startPoint(0) != nullptr);
        REQUIRE(world.items().loaded());
        REQUIRE(world.powerups().loaded());
        world.update(1.0f / 30);
        const auto camera = world.entranceCamera();
        REQUIRE(camera);
        device.draws.clear();
        world.draw(device, Mat4{1}, *camera);
        REQUIRE_FALSE(device.draws.empty());
        device.draws.clear();
        world.clear();
        CHECK_FALSE(world.built());
        CHECK_FALSE(world.collision().loaded());
        CHECK(world.level() == nullptr);
        ++loaded;
    }
    CHECK(loaded == available.size());
}

} // namespace
