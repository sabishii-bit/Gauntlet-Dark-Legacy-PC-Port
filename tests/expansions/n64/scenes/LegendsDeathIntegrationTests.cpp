#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/enemies/LegendsEnemyAssets.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/enemies/EnemyKinds.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("Boss1 Death statue remains solid until the common pool accepts its live native actor",
          "[legends][native-death][assets][scene-adapter]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("N64 Legends extraction unavailable");
    }
    test::FakeRenderDevice device;
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto targets = scene->deathTargets();
    REQUIRE_FALSE(targets.empty());
    const auto target = targets.front();
    Enemies enemies;
    std::vector<std::string> warnings;
    const auto program = readFile(root / "_program/game.bin");
    const auto openPool = [&] {
        enemies.open(device, {}, nullptr, 1, {}, 1);
        REQUIRE(enemies.setArchiveLoader(
            [&](s32 kind) { return loadLegendsEnemyAssets(root, kind, program, warnings); }));
        REQUIRE(enemies.loadKind(kDeathKind));
    };
    openPool();
    // Ordinary actors, including Death, may be recycled by the common pool.
    // It is explicitly non-recyclable, so it exercises a genuine refusal.
    REQUIRE(enemies.loadKind(kItKind));
    REQUIRE(enemies.spawn({.kind = kItKind, .position = {1000, 0, 0}, .placed = true}, {}, {}));
    REQUIRE(target.valid());
    REQUIRE(scene->deathObstacles().front().touchedBy(target.base, 1));
    const auto object = scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase);
    REQUIRE(object);
    CHECK(object->name == "POWERUP:DETH");
    CHECK(object->visible);
    usize attempts = 0;
    scene->releaseDeaths([&](const EnemySpawn&) {
        ++attempts;
        return true;
    });
    CHECK(attempts == 0);
    CHECK_FALSE(scene->takeDeathAdvice());
    SECTION("touch") {
        scene->touchDeaths(target.base, 1, 3);
    }
    SECTION("hit") {
        REQUIRE(scene->strikeDeath(target.id, 1));
    }
    scene->releaseDeaths([&](const EnemySpawn& spawn) {
        ++attempts;
        return enemies.spawn(spawn, {}, {}).has_value();
    });
    CHECK(attempts == 1);
    CHECK_FALSE(scene->takeDeathAdvice());
    CHECK(scene->deathTargets().size() == targets.size());
    const auto refused = scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase);
    REQUIRE(refused);
    CHECK(refused->visible);
    scene->takeSounds();
    enemies.close();
    openPool();
    scene->releaseDeaths([&](const EnemySpawn& spawn) {
        ++attempts;
        CHECK(spawn.kind == kDeathKind);
        CHECK(spawn.position == target.base);
        return enemies.spawn(spawn, {}, {}).has_value();
    });
    CHECK(attempts == 2);
    CHECK(scene->takeDeathAdvice());
    CHECK_FALSE(scene->takeDeathAdvice());
    REQUIRE(enemies.count() == 1);
    REQUIRE(enemies.animatorOf(0));
    CHECK(enemies.animatorOf(0)->bound());
    device.draws.clear();
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    CHECK(scene->deathTargets().size() + 1 == targets.size());
    // Console identities survive state changes; the dormant artwork is hidden,
    // not removed from the archive or accidentally renumbered to another item.
    const auto released = scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase);
    REQUIRE(released);
    CHECK(released->name == "POWERUP:DETH");
    CHECK_FALSE(released->visible);
    device.draws.clear();
    scene->highlightFixture(device, Mat4{1}, CameraFrame::of(WorldCamera{}), 1,
                            LegendsSceneAdapter::kFixtureObjectBase, SelectionHighlight{Vec3{0}});
    CHECK(device.draws.empty());
    const auto sounds = scene->takeSounds();
    REQUIRE(sounds.size() == 1);
    CHECK(sounds.front().token == 0x6e);
    scene->releaseDeaths([&](const EnemySpawn&) {
        ++attempts;
        return true;
    });
    CHECK(attempts == 2);
    CHECK(scene->takeSounds().empty());
    CHECK_FALSE(scene->takeDeathAdvice());
}

TEST_CASE("Native Death advice is once per party rather than per map or saved character",
          "[legends][native-death-advice]") {
    auto session = std::make_shared<LegendsAdviceState>();
    auto nextMap = session;
    CHECK_FALSE(session->admitDeath(11, 10));
    CHECK_FALSE(session->deathShown);
    CHECK_FALSE(session->admitDeath(0, 10));
    CHECK_FALSE(session->admitDeath(-1, 10));
    CHECK(nextMap->admitDeath(10, 10));
    CHECK_FALSE(session->admitDeath(1, 10));
    // Mirrors successful app player-select entry, including any currently held
    // native scene. Merely replacing the app pointer would leave that scene stale.
    *session = {};
    CHECK(nextMap->admitDeath(1, 10));
    CHECK_FALSE(session->admitDeath(10, 10));
}

TEST_CASE("Actual native Death release queues the eligible player's modal advice and voice once",
          "[legends][native-death][native-death-advice][native-speech][assets]") {
    const auto level = GENERATE(10, 11);
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad") ||
        !std::filesystem::is_regular_file(root / "audio/mort.mdir")) {
        SKIP("N64 Legends map and speech extraction unavailable");
    }
    test::FakeRenderDevice device;
    const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                   readFile(directory / "objects.wad"),
                                                   readFile(directory / "ipr.rom"));
    const auto session = std::make_shared<LegendsAdviceState>();
    const auto load = [&] {
        auto decoded = map;
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, decoded);
        return std::make_unique<LegendsLevel>(device, decoded, std::move(adapter), directory,
                                              root / "no-gc-data", session);
    };
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    auto native = load();
    CharacterSave save;
    save.progress().experience = levelExperience(level);
    save.progress().health = 500;
    PlayerActor player;
    player.spawn(2, save, nullptr, {1000, 0, 1000}, 0);
    const auto initialSave = player.save();
    std::array<PlayerActor*, 1> players{&player};
    const auto targets = native->adapter().deathTargets();
    REQUIRE_FALSE(targets.empty());
    REQUIRE(native->strike(targets.front().id, 1, 0, 2));
    native->releaseEnemies([](const EnemySpawn&) { return false; });
    native->updateParty(players, 1.f / 60, false);
    CHECK(native->takePickups().empty());
    CHECK_FALSE(session->deathShown);
    native->releaseEnemies([](const EnemySpawn&) { return true; });
    // Drain only the separate native spawn effect before observing narration.
    native->playSounds(output, {1000, 0, 1000});
    native->stopSounds();
    std::array<f32, 2048> stopped{};
    mixer.mix(stopped); // The common SoundPlayer stop has a short release ramp.
    output.update();
    native->updateParty(players, 1.f / 60, false);
    const auto receipts = native->takePickups();
    CHECK(player.save() == initialSave);
    if (level > 10) {
        CHECK(receipts.empty());
        CHECK_FALSE(session->deathShown);
    } else {
        REQUIRE(receipts.size() == 1);
        CHECK(receipts.front().player == 2);
        CHECK_FALSE(receipts.front().scrollKeys);
        CHECK(receipts.front().scrollPages ==
              std::vector<std::string>{"Use potions to Kill Death"});
        CHECK(session->deathShown);
        native->playSounds(output, {1000, 0, 1000});
        REQUIRE(output.voiceCount() == 1);
        std::array<f32, 1600> samples{};
        f64 energy = 0;
        for (usize tick = 0; tick < 900; ++tick) {
            native->update(1.f / 60, false);
            native->playSounds(output, {1000, 0, 1000});
            mixer.mix(samples);
            for (const f32 value : samples)
                energy += std::abs(value);
            output.update();
        }
        CHECK(energy > 1);
        CHECK(output.voiceCount() == 0);
    }
    native->stopSounds();
    // A high-level-only release does not spend this new party's advice. A later
    // low-level visitor remains eligible; already-advised parties stay quiet.
    native = load();
    player.save().progress().experience = levelExperience(1);
    const auto nextTargets = native->adapter().deathTargets();
    REQUIRE_FALSE(nextTargets.empty());
    REQUIRE(native->strike(nextTargets.front().id, 1, 0, 2));
    native->releaseEnemies([](const EnemySpawn&) { return true; });
    native->updateParty(players, 1.f / 60, false);
    CHECK(native->takePickups().size() == (level > 10 ? 1u : 0u));
    CHECK(session->deathShown);
    native->updateParty(players, 1.f / 60, false);
    CHECK(native->takePickups().empty());
    native->stopSounds();
}

TEST_CASE("Death advice enters the normal PlayScene scroll and holds party controls",
          "[legends][native-death-advice][imported-play][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("N64 Legends extraction unavailable");
    const auto characters = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    auto level = std::make_shared<LegendsLevel>(
        device, map, LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map),
        directory, characters);
    CharacterSave save;
    save.progress().health = 9000;
    std::array party{PartyMember{2, save}};
    level->initializeParty(party);
    ClassDataSet classes;
    REQUIRE(classes.load(characters / "pdata"));
    LegendsTraversal traversal;
    traversal.open(map, 2, save, classes.stats(save.character));
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.realm = "Legends";
    ref.name = "n64:boss1";
    REQUIRE(world.loadImported(
        device, characters, ref,
        WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Start, .position = traversal.start()}},
                                 map.minBounds, map.maxBounds),
        traversal.collision(), level));
    GameConfig config;
    config.camera.compass = false;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    AssetLocator assets{characters};
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.unpackedRoot = characters;
    context.sounds = &output;
    PlayOptions options;
    options.position = traversal.start();
    options.yaw = traversal.actor().yaw();
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.animator(2));
    REQUIRE(scene.actor(2));
    for (usize tick = 0; tick < 1200 && (scene.spawning() || !level->opponentsReady() ||
                                         scene.animator(2)->entering());
         ++tick) {
        REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE(level->opponentsReady());
    REQUIRE_FALSE(scene.scroll().active());
    const auto statues = level->adapter().deathTargets();
    REQUIRE_FALSE(statues.empty());
    REQUIRE(level->strike(statues.front().id, 1, 0, 2));
    // Normal updateEnemies, not a test-only spawn callback, admits the live Death.
    for (usize tick = 0; tick < 5 && !scene.scroll().active(); ++tick)
        REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
    REQUIRE(scene.scroll().active());
    CHECK(scene.scroll().lines() == std::vector<std::string>{"Use potions to Kill Death"});
    CHECK(level->adapter().deathTargets().size() + 1 == statues.size());
    const Vec3 held = scene.actor(2)->position();
    PlayScene::Inputs moving{};
    moving[2].move = {{1, 0}, 1};
    for (usize tick = 0; tick < 30; ++tick) {
        REQUIRE(scene.update(1.0 / 60, moving) == PlayOutcome::Running);
        CHECK(scene.actor(2)->position() == held);
    }
    CHECK(scene.scroll().active());
    scene.close();
}

TEST_CASE("Native contained Death waits for opening and never becomes an inventory reward",
          "[legends][native-death][containers][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/castle6";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("N64 Legends extraction unavailable");
    }
    test::FakeRenderDevice device;
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto containers = scene->containerTargets();
    const auto found = std::ranges::find(containers, 4u, &LegendsContainerTarget::nativeIndex);
    REQUIRE(found != containers.end());
    // Independent authored IPR value: container4, contents key0x2101, XZ(-5,48)/4.
    CHECK(found->base.x == -1.25f);
    CHECK(found->base.z == 12);
    const auto before = scene->deathTargets().size();
    usize births = 0;
    const auto release = [&](const EnemySpawn& spawn) {
        ++births;
        CHECK(spawn.kind == kDeathKind);
        CHECK(spawn.position.x == found->base.x);
        CHECK(spawn.position.z == found->base.z);
        return true;
    };
    scene->releaseDeaths(release);
    CHECK(births == 0);
    if (found->breakable) {
        REQUIRE(scene->strikeContainer(found->id, 10000, 0, 0));
    } else {
        CharacterSave save;
        save.progress().inventory.keys = 1;
        PlayerActor player;
        player.spawn(0, save, nullptr, found->base, 0);
        scene->collect(player, 1, false);
        CHECK(player.save().progress().inventory.keys == 0);
    }
    scene->releaseDeaths(release);
    CHECK(births == 0);
    for (s32 tick = 0; tick < 360; ++tick) {
        scene->update(1.f / 60);
        scene->releaseDeaths(release);
    }
    CHECK(births == 1);
    CHECK(scene->deathTargets().size() == before);
    scene->releaseDeaths(release);
    CHECK(births == 1);
}
