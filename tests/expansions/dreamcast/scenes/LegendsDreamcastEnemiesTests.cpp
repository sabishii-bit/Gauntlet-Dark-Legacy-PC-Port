#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/math/Mt19937.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/enemies/LegendsDreamcastEnemies.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/ScriptedOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::exists(root / "levels/levelb1/worlds.rom")) {
        SKIP("Optional Dreamcast assets are not installed");
    }
    return root;
}

void checkGeneratorArtwork(test::FakeRenderDevice& device, const LegendsSceneAdapter& scene,
                           s32 selected, ItemArchive& archive, std::string_view modelName,
                           usize triangles, u32 textureSize) {
    const auto model = archive.models.find(modelName);
    REQUIRE(model);
    const auto& mesh = archive.models.mesh(*model);
    REQUIRE(mesh.triangleCount() == triangles);
    device.draws.clear();
    scene.highlightFixture(device, Mat4{1}, CameraFrame{}, 1, selected,
                           SelectionHighlight{Vec3{0, 100, 0}});
    REQUIRE_FALSE(device.draws.empty());
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += draw.vertices.size();
        const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
        REQUIRE(texture);
        CHECK(texture->width() == textureSize);
        CHECK(texture->height() == textureSize);
        // Match the actual submitted pixels to the selected native model's
        // resident textures, not merely an enlarged low-detail fallback.
        CHECK(std::ranges::any_of(mesh.parts, [&](const auto& part) {
            return texture->pixels == archive.textures.image(part.texture).pixels;
        }));
    }
    // The console highlight emits the visible geometry twice: rim + wash.
    CHECK(vertices == triangles * 3 * 2);
}

TEST_CASE("Dreamcast B1 admits all native troll generators and ranged variants",
          "[legends][dreamcast-enemies][assets]") {
    const auto root = nativeRoot();
    const auto level = root / "levels/levelb1";
    const auto native = formats::LegendsDreamcastScene::parse(readFile(level / "worlds.rom"));
    const auto scope =
        formats::dreamcastLegendsFixtureScope(readFile(root / "worlddata/mount.wad"), "levelb1");
    REQUIRE(scope);
    usize generators = 0;
    usize actors = 0;
    std::array<usize, 6> variants{};
    for (usize i = 0; i < native.itemInstances.size(); ++i) {
        const auto& item = native.itemInstances[i];
        const auto& info = native.itemInfos[static_cast<usize>(item.info)];
        std::string warning;
        if (info.type == 3) {
            const auto value =
                legendsDreamcastGenerator(info, item, static_cast<u32>(i), *scope, &warning);
            INFO(i << ": " << warning);
            REQUIRE(value);
            CHECK(value->gameplay.kind == enemy_type::kTroll);
            CHECK(value->gameplay.body.hitPoints == 10);
            ++generators;
        } else if (info.type == 4) {
            const auto value =
                legendsDreamcastActor(info, item, static_cast<u32>(i), *scope, &warning);
            INFO(i << ": " << warning);
            REQUIRE(value);
            REQUIRE(value->spawn.tier >= 1);
            REQUIRE(value->spawn.tier <= 6);
            ++variants[static_cast<usize>(value->spawn.tier - 1)];
            ++actors;
        }
    }
    CHECK(generators == 26);
    CHECK(actors == 20);
    CHECK(variants == std::array<usize, 6>{0, 1, 0, 12, 5, 2});

    test::FakeRenderDevice device;
    auto map = formats::LegendsMap::parseDreamcast(readFile(level / "worlds.rom"),
                                                   readFile(level / "objects.rom"));
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, level, map, 4);
    REQUIRE(scene);
    CHECK(scene->generators().size() == generators);
    CHECK(scene->actors().size() == actors);
}

TEST_CASE(
    "Dreamcast full-health generators retain their native tier artwork until damaged",
    "[legends][dreamcast-enemies][dreamcast-presentation][dreamcast-generator-banks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb1";
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                   readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    auto species = loadLegendsDreamcastArchive(root / "monsters/tro");
    REQUIRE(species);
    std::array<usize, 3> tiers{};
    for (const auto& generator : scene->generators()) {
        REQUIRE(generator.tier >= 1);
        REQUIRE(generator.tier <= 3);
        ++tiers[static_cast<usize>(generator.tier - 1)];
    }
    // SetItem 8c0a8340 takes the initial visual state from placement+0x30.
    // The thirteen small huts are authored TRO1, not prematurely damaged TRO3.
    CHECK(tiers == std::array<usize, 3>{13, 9, 4});
    const auto full = std::ranges::find(scene->generators(), 69U, &ImportedGenerator::nativeIndex);
    REQUIRE(full != scene->generators().end());
    REQUIRE(full->tier == 3);
    s32 selected = -1;
    for (usize index = 0; index < scene->fixtureCount(); ++index) {
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto object = scene->fixtureObject(id);
        if (object && object->name.starts_with("GEN_TRO3") &&
            glm::length(Vec2{object->position.x - full->position.x,
                             object->position.z - full->position.z}) < .001f) {
            selected = id;
            break;
        }
    }
    REQUIRE(selected >= 0);

    Enemies enemies;
    enemies.open(device, root, nullptr, 32, {}, 4);
    std::vector<std::string> warnings;
    REQUIRE(enemies.setArchiveLoader(
        [&](s32 kind) { return loadLegendsDreamcastEnemyAssets(root, kind, warnings); }));
    Generators generators;
    REQUIRE(generators.bind(device, WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                            enemies, nullptr, {}, 4));
    const auto pool = generators.spawnImported(device, enemies, *full);
    REQUIRE(pool);
    CHECK(generators.healthOf(*pool) == 30);
    const auto verifyArtwork = [&](s32 expectedState, std::string_view model, usize triangles,
                                   u32 textureSize) {
        CHECK(generators.stateOf(*pool) == expectedState);
        const std::array states{
            ImportedGeneratorState{full->nativeIndex, *pool, generators.stateOf(*pool),
                                   generators.positionOf(*pool), generators.standing(*pool)}};
        scene->updateGenerators(states);
        checkGeneratorArtwork(device, *scene, selected, *species, model, triangles, textureSize);
    };
    // Active retail play loads the species bank (LoadWorld 8c0a3580), not the
    // generic inspection-mode bank. Its tier-two model has a distinct ROOT
    // spelling/topology that must not lose to generic GEN_TRO2L1.
    verifyArtwork(3, "GEN_TRO3L1", 22, 128);
    // HitItem 8c0afaa0 changes state only at templateHP and 2*templateHP.
    REQUIRE(generators.strike(*pool, static_cast<f32>(full->body.armor) + 9, 0));
    CHECK(generators.healthOf(*pool) == 21);
    verifyArtwork(3, "GEN_TRO3L1", 22, 128);
    REQUIRE(generators.strike(*pool, static_cast<f32>(full->body.armor) + 1, 0));
    verifyArtwork(2, "GEN_TRO2L1ROOT", 26, 32);
    REQUIRE(generators.strike(*pool, static_cast<f32>(full->body.armor) + 10, 0));
    verifyArtwork(1, "GEN_TRO1L1", 28, 32);
    REQUIRE(generators.strike(*pool, static_cast<f32>(full->body.armor) + 10, 0));
    verifyArtwork(0, "GEN_TRO0L1", 34, 32);
}

TEST_CASE(
    "Dreamcast B1 reported TRO1 hut binds its native species artwork",
    "[legends][dreamcast-enemies][dreamcast-presentation][dreamcast-generator-banks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb1";
    const auto worlds = readFile(directory / "worlds.rom");
    auto map = formats::LegendsMap::parseDreamcast(worlds, readFile(directory / "objects.rom"));
    const auto native = formats::WorldFile::parse(worlds);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 1);
    REQUIRE(scene);
    constexpr s32 kReported = 16777265;
    const auto object = scene->fixtureObject(kReported);
    REQUIRE(object);
    REQUIRE(object->name == "GEN_TRO1L1");
    const auto generator = std::ranges::find_if(scene->generators(), [&](const auto& candidate) {
        return glm::length(Vec2{candidate.position.x - object->position.x,
                                candidate.position.z - object->position.z}) < .001f;
    });
    REQUIRE(generator != scene->generators().end());
    REQUIRE(generator->nativeIndex < native.itemInstances.size());
    CHECK(native.itemInstances[generator->nativeIndex].params[0] == 1);
    CHECK(generator->tier == 1);
    CHECK(object->visible);
    auto species = loadLegendsDreamcastArchive(root / "monsters/tro");
    REQUIRE(species);
    checkGeneratorArtwork(device, *scene, kReported, *species, "GEN_TRO1L1", 28, 32);
}

TEST_CASE("Dreamcast generator artwork falls back when its optional species bank is absent",
          "[legends][dreamcast-enemies][dreamcast-generator-banks][assets]") {
    const auto original = nativeRoot();
    const auto root = test::scratchDirectory("dreamcast-generator-missing-species");
    // Copy only this map's native fixtures and generic fallback (under 2 MB),
    // deliberately excluding every monster, item, sound and GameCube archive.
    for (const auto* const name :
         {"levels/levelb1/worlds.rom", "levels/levelb1/objects.rom", "levels/levelb1/textures.rom",
          "worlddata/mount.wad", "generators/objects.rom", "generators/textures.rom"}) {
        const auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        writeFile(path, readFile(original / name));
    }
    REQUIRE_FALSE(std::filesystem::exists(root / "monsters"));
    const auto directory = root / "levels/levelb1";
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                   readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    REQUIRE(scene->generators().size() == 26);
    CHECK(std::ranges::count(
              scene->warnings(),
              "Dreamcast generator library is unavailable: tro; using fallback artwork") == 1);
    const auto full = std::ranges::find(scene->generators(), 69U, &ImportedGenerator::nativeIndex);
    REQUIRE(full != scene->generators().end());
    REQUIRE(full->tier == 3);
    s32 selected = -1;
    for (usize index = 0; index < scene->fixtureCount(); ++index) {
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto object = scene->fixtureObject(id);
        if (object && object->name == "GEN_TRO3L1" &&
            glm::length(Vec2{object->position.x - full->position.x,
                             object->position.z - full->position.z}) < .001f) {
            selected = id;
            break;
        }
    }
    REQUIRE(selected >= 0);
    auto fallback = loadLegendsDreamcastArchive(root / "generators");
    REQUIRE(fallback);
    constexpr std::array kModels{"GEN_TRO0L1", "GEN_TRO1L1", "GEN_TRO2L1", "GEN_TRO3L1"};
    constexpr std::array<usize, 4> kTriangles{34, 28, 28, 22};
    constexpr std::array<u32, 4> kSizes{16, 16, 16, 32};
    for (s32 state = 3; state >= 0; --state) {
        CAPTURE(state);
        const auto index = static_cast<usize>(state);
        const std::array states{
            ImportedGeneratorState{full->nativeIndex, 0, state, full->position, state > 0}};
        scene->updateGenerators(states);
        checkGeneratorArtwork(device, *scene, selected, *fallback, kModels[index],
                              kTriangles[index], kSizes[index]);
    }
}

TEST_CASE("Dreamcast B4 demon generators borrow lava without replacing resident species textures",
          "[legends][dreamcast-enemies][dreamcast-generator-banks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb4";
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                   readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    const auto generator =
        std::ranges::find(scene->generators(), enemy_type::kDemon, &ImportedGenerator::kind);
    REQUIRE(generator != scene->generators().end());
    s32 selected = -1;
    std::string name;
    for (usize index = 0; index < scene->fixtureCount(); ++index) {
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto object = scene->fixtureObject(id);
        if (object && object->visible && object->name.starts_with("GEN_DEM") &&
            glm::length(Vec2{object->position.x - generator->position.x,
                             object->position.z - generator->position.z}) < .001f) {
            selected = id;
            name = object->name;
            break;
        }
    }
    REQUIRE(selected >= 0);
    auto species = loadLegendsDreamcastArchive(root / "monsters/dem");
    auto level = loadLegendsDreamcastArchive(directory);
    REQUIRE(species);
    REQUIRE(level);
    const auto model = species->models.find(name);
    REQUIRE(model);
    const auto& mesh = species->models.mesh(*model);
    struct ExpectedTexture {
        const Image* image = nullptr;
        bool external = false;
    };
    std::vector<ExpectedTexture> expected;
    for (const auto& part : mesh.parts) {
        const auto& entry = species->textures.entry(part.texture);
        if (entry.external()) {
            REQUIRE(entry.name == "LAVA");
            const auto source = level->textures.find(entry.name);
            REQUIRE(source);
            REQUIRE_FALSE(level->textures.entry(*source).external());
            const auto& image = level->textures.image(*source);
            REQUIRE_FALSE(image.pixels.empty());
            expected.push_back({&image, true});
        } else {
            const auto& image = species->textures.image(part.texture);
            REQUIRE_FALSE(image.pixels.empty());
            expected.push_back({&image, false});
        }
    }
    REQUIRE(std::ranges::any_of(expected, &ExpectedTexture::external));
    REQUIRE(std::ranges::any_of(expected, [](const auto& texture) { return !texture.external; }));
    device.draws.clear();
    scene->highlightFixture(device, Mat4{1}, CameraFrame{}, 1, selected,
                            SelectionHighlight{Vec3{0, 100, 0}});
    REQUIRE_FALSE(device.draws.empty());
    bool sawLava = false;
    bool sawResident = false;
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += draw.vertices.size();
        const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
        REQUIRE(texture);
        const auto source = std::ranges::find_if(expected, [&](const auto& entry) {
            return texture->width() == entry.image->width &&
                   texture->height() == entry.image->height &&
                   texture->pixels == entry.image->pixels;
        });
        REQUIRE(source != expected.end());
        sawLava |= source->external;
        sawResident |= !source->external;
    }
    CHECK(sawLava);
    CHECK(sawResident);
    CHECK(vertices == mesh.triangleCount() * 3 * 2);
}

TEST_CASE("Dreamcast B1 breeds native enemies through the actual imported level",
          "[legends][dreamcast-enemies][dreamcast-opponents][assets]") {
    const auto directory = nativeRoot() / "levels/levelb1";
    const auto common = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                   readFile(directory / "objects.rom"));
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    const usize expected = adapter->generators().size();
    REQUIRE(expected > 0);
    auto native =
        std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
    REQUIRE(native->hasEnemyAssets());
    LevelWorld world;
    LevelRef reference;
    reference.realmId = -1;
    reference.name = "dreamcast:levelb1";
    REQUIRE(world.loadImported(device, common, reference,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), native));
    ItemArchive weapons;
    REQUIRE(weapons.load(common / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, traversal.actor().position(), 0);
    players[0].actor.save().progress().health = 10000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, common}, players);
    REQUIRE(opponents.generators().count() == expected);
    for (const auto& state : opponents.importedGeneratorStates()) {
        REQUIRE(state.pool >= 0);
        CHECK(state.standing);
    }
    test::ScriptedOpponents host;
    s32 child = -1;
    for (s32 tick = 0; tick < 300 && child < 0; ++tick) {
        opponents.update(2, 1.f / 30, players, {}, host, host, host);
        for (usize id = 0; id < opponents.enemies().count(); ++id) {
            if (opponents.enemies().generatorOf(static_cast<s32>(id)) >= 0) {
                child = static_cast<s32>(id);
                break;
            }
        }
    }
    REQUIRE(child >= 0);
    CHECK(opponents.enemies().alive(child));
    CHECK(opponents.enemies().kindOf(child) == enemy_type::kTroll);
    CHECK(world.collision().floorAt(opponents.enemies().positionOf(child), 2, 2));
    const auto* artwork = opponents.enemies().archiveOf(enemy_type::kTroll);
    REQUIRE(artwork);
    CHECK(artwork->trees.find("TRO1"));
    CHECK(artwork->trees.find("TROA"));
    CHECK(artwork->trees.find("TROB"));
    CHECK(artwork->trees.find("TROS"));
    opponents.enemies().draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
}

TEST_CASE("Dreamcast Temple and Hell generators bind their native mixed broods",
          "[legends][dreamcast-enemies][dreamcast-mixed-generators][assets]") {
    const auto root = nativeRoot();
    for (const bool hell : {false, true}) {
        const std::string code = hell ? "levelf1" : "levele1";
        CAPTURE(code);
        const auto directory = root / "levels" / code;
        const auto sceneBytes = readFile(directory / "worlds.rom");
        const auto decoded = formats::LegendsDreamcastScene::parse(sceneBytes);
        const auto scope = formats::dreamcastLegendsFixtureScope(
            readFile(root / "worlddata" / (hell ? "hell.wad" : "temple.wad")), code);
        REQUIRE(scope);
        const std::vector<s32> kinds =
            hell ? std::vector<s32>{5, 2, 8, 11} : std::vector<s32>{4, 1, 7, 10};
        const std::vector<s32> ways =
            hell ? std::vector<s32>{7, 30, 0, 0} : std::vector<s32>{7, 7, 0, 7};
        const s32 tier = hell ? 3 : 2;
        usize authored = 0;
        for (usize index = 0; index < decoded.itemInstances.size(); ++index) {
            const auto& item = decoded.itemInstances[index];
            const auto& info = decoded.itemInfos[static_cast<usize>(item.info)];
            if (info.type != 3) {
                continue;
            }
            const auto generator =
                legendsDreamcastGenerator(info, item, static_cast<u32>(index), *scope);
            REQUIRE(generator);
            CHECK(generator->gameplay.tier == tier);
            CHECK(generator->gameplay.broodTier == tier);
            CHECK(generator->gameplay.broodKinds == kinds);
            CHECK(generator->gameplay.broodAlgorithms == ways);
            CHECK(generator->models[static_cast<usize>(tier)] ==
                  "GEN_SPECIAL" + std::to_string(tier));
            ++authored;
        }
        CHECK(authored == (hell ? 167 : 98));
        test::FakeRenderDevice device;
        auto map =
            formats::LegendsMap::parseDreamcast(sceneBytes, readFile(directory / "objects.rom"));
        auto scene =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
        REQUIRE(scene);
        REQUIRE(scene->generators().size() == authored);

        std::vector<std::string> warnings;
        Enemies enemies;
        enemies.open(device, root, nullptr, 32, {}, 4);
        REQUIRE(enemies.setArchiveLoader([&](s32 kind) {
            return loadLegendsDreamcastEnemyAssets(root, kind, warnings, scope->enemySounds);
        }));
        Generators generators;
        REQUIRE(generators.bind(device,
                                WorldLayout::fromDecoded({}, {}, {-100, -1, -100}, {100, 10, 100}),
                                enemies, nullptr, {}, 4));
        auto definition = scene->generators().front();
        definition.position = {0, 0, 0};
        definition.minimumPlayers = 1;
        definition.spawnInterval = 1;
        definition.maximumBrood = 1;
        definition.alwaysActive = true;
        // This case isolates native species/strategy RNG. Actual native
        // quota and wait ranges are exercised by the cadence cases below.
        definition.breeding.reset();
        REQUIRE(generators.spawnImported(device, enemies, definition));
        CHECK(generators.healthOf(0) == static_cast<f32>(10 * tier));
        for (const s32 kind : kinds) {
            const auto* archive = enemies.archiveOf(kind);
            REQUIRE(archive);
            // The source bank contains only its selected tier, not an ordinary
            // species library with a coincidentally similarly named body.
            CHECK(archive->trees.find(std::string{enemyKind(kind).prefix} + std::to_string(tier)));
            CHECK_FALSE(archive->trees.find(std::string{enemyKind(kind).prefix} + "1"));
        }
        CHECK(warnings.empty());
        const std::array views{EnemyView{.player = 0, .position = {0, 0, 15}}};
        Mt19937 expected(0x4c454745U);
        std::set<s32> seen;
        for (s32 birth = 0; birth < 24; ++birth) {
            // Only one generator attempts a birth, with an empty flat arena.
            // This pins four-way RNG selection and native algorithms directly.
            generators.update(1000, enemies, views, {}, true);
            const usize choice = ((expected() >> 17U) * 4U) >> 15U;
            generators.update(2, enemies, views);
            s32 child = -1;
            for (s32 id = 0; id < Enemies::kMost; ++id) {
                if (enemies.alive(id) && !enemies.dying(id)) {
                    REQUIRE(child < 0);
                    child = id;
                }
            }
            REQUIRE(child >= 0);
            CHECK(enemies.kindOf(child) == kinds[choice]);
            CHECK(enemies.tierOf(child) == tier);
            if (ways[choice] != 0) {
                CHECK(enemies.algorithmOf(child) == ways[choice]);
            }
            seen.insert(enemies.kindOf(child));
            device.draws.clear();
            enemies.draw(device, Mat4{1}, WorldLighting{});
            CHECK_FALSE(device.draws.empty());
            enemies.hurt(child, EnemyHit{.damage = 10000, .where = std::nullopt});
            enemies.update(240, 4, views);
        }
        CHECK(seen == std::set<s32>(kinds.begin(), kinds.end()));
        // A missing optional selected archive must fail closed, not load an
        // unrelated base model. The normal three-species path is tested below.
        CHECK_FALSE(loadLegendsDreamcastEnemyAssets(root / "absent", kinds.front(), warnings,
                                                    scope->enemySounds));
    }
}

TEST_CASE("Dreamcast E1 and F1 spawn selected-tier enemies through LevelOpponents",
          "[legends][dreamcast-opponents][dreamcast-mixed-generators][assets]") {
    const auto root = nativeRoot();
    const auto common = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    for (const std::string code : {"levele1", "levelf1"}) {
        CAPTURE(code);
        const auto directory = root / "levels" / code;
        auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                       readFile(directory / "objects.rom"));
        LegendsTraversal traversal;
        traversal.open(map, 0, CharacterSave{});
        test::FakeRenderDevice device;
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
        REQUIRE(adapter);
        const usize expected = adapter->generators().size();
        REQUIRE(expected > 0);
        const auto definition = adapter->generators().front();
        auto native =
            std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
        LevelWorld world;
        LevelRef reference;
        reference.realmId = -1;
        reference.name = "dreamcast:" + code;
        REQUIRE(world.loadImported(device, common, reference,
                                   WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                                   traversal.collision(), native));
        ItemArchive weapons;
        REQUIRE(weapons.load(common / "WEAPONS"));
        EffectTrees effects;
        LevelSoundscape audio;
        std::array<PlayerRuntime, 1> players;
        players[0].actor.spawn(0, {}, nullptr, definition.position + Vec3{0, 0, 12}, 0);
        players[0].actor.save().progress().health = 10000;
        LevelOpponents opponents;
        opponents.open({device, world, weapons, effects, audio, common}, players);
        REQUIRE(opponents.generators().count() == expected);
        for (const auto& state : opponents.importedGeneratorStates()) {
            REQUIRE(state.pool >= 0);
            CHECK(opponents.generators().mostOf(state.pool) == (code == "levelf1" ? 4 : 7));
        }
        test::ScriptedOpponents host;
        s32 child = -1;
        for (s32 tick = 0; tick < 600 && child < 0; ++tick) {
            opponents.update(2, 1.f / 30, players, {}, host, host, host);
            for (s32 id = 0; id < Enemies::kMost; ++id) {
                if (opponents.enemies().alive(id) && opponents.enemies().generatorOf(id) >= 0) {
                    child = id;
                    break;
                }
            }
        }
        REQUIRE(child >= 0);
        const s32 kind = opponents.enemies().kindOf(child);
        CHECK(std::ranges::find(definition.broodKinds, kind) != definition.broodKinds.end());
        CHECK(opponents.enemies().tierOf(child) == definition.broodTier);
        const auto* archive = opponents.enemies().archiveOf(kind);
        REQUIRE(archive);
        CHECK(archive->trees.find(std::string{enemyKind(kind).prefix} +
                                  std::to_string(definition.broodTier)));
        CHECK_FALSE(archive->trees.find(std::string{enemyKind(kind).prefix} + "1"));
        opponents.enemies().draw(device, Mat4{1}, WorldLighting{});
        CHECK_FALSE(device.draws.empty());
    }
}
} // namespace
