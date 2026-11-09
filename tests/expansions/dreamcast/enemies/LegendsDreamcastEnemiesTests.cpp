#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
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
    if (!std::filesystem::exists(root / "levels/levelb1/worlds.rom"))
        SKIP("Optional Dreamcast assets are not installed");
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

TEST_CASE("Dreamcast generators preserve native template HP and placed strategies",
          "[legends][dreamcast-enemies]") {
    formats::ItemInfoRecord info;
    info.type = 3;
    info.name = "GRU";
    info.collisionType = 1;
    info.radius = 2;
    info.height = 5;
    info.hitPoints = 10;
    formats::ItemInstanceRecord item;
    item.minPlayers = 2;
    item.flags = 1;
    item.rotation.y = glm::radians(90.f);
    item.params[0] = 3;
    item.params[2] = 7;
    item.params[4] = 5;
    item.params[6] = 10;
    formats::LegendsDreamcastFixtureScope scope;
    scope.realmType = 1;
    scope.enemyClasses[2] = 1;
    scope.enemyClasses[4] = 4;
    auto generator = legendsDreamcastGenerator(info, item, 42, scope);
    REQUIRE(generator);
    CHECK(generator->gameplay.kind == enemy_type::kTroll);
    CHECK(generator->gameplay.minimumPlayers == 2);
    CHECK(generator->gameplay.tier == 3);
    CHECK(generator->gameplay.body.hitPoints == 10);
    CHECK(generator->gameplay.stageHealth == std::array<s32, 3>{});
    CHECK(generator->gameplay.algorithm == 7);
    // SetItem overwrites byte4 and does not read byte6 for generators;
    // these are not Dark Legacy's authored quota/interval overrides.
    CHECK(generator->gameplay.maximumBrood == 0);
    CHECK(generator->gameplay.spawnInterval == 0);
    REQUIRE(generator->gameplay.breeding);
    CHECK(generator->gameplay.breeding->broodByState == std::array<s32, 3>{6, 4, 2});
    CHECK(generator->gameplay.breeding->waitTicks == std::array<s32, 2>{30, 90});
    CHECK(generator->gameplay.breeding->broodBonus == 0);
    CHECK(generator->gameplay.alwaysActive);
    CHECK(generator->gameplay.yaw == -item.rotation.y);
    CHECK(generator->models ==
          std::array<std::string, 4>{"GEN_TRO0", "GEN_TRO1", "GEN_TRO2", "GEN_TRO3"});

    info.type = 4;
    item.params = {};
    item.params[0] = 4;
    item.params[2] = 23;
    const auto actor = legendsDreamcastActor(info, item, 43, scope);
    REQUIRE(actor);
    CHECK(actor->spawn.kind == enemy_type::kGrunt);
    CHECK(actor->spawn.tier == 4);
    CHECK(actor->spawn.algorithm == 23);
    CHECK(actor->minimumPlayers == 2);
    CHECK(actor->yaw == item.rotation.y);
    CHECK(actor->idleYaw == item.rotation.y);
    CHECK(std::abs(actor->spawn.direction.x - 1) < .00001f);
    CHECK(std::abs(actor->spawn.direction.z) < .00001f);
    item.position.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(legendsDreamcastActor(info, item, 43, scope));
    info.type = 3;
    CHECK_FALSE(legendsDreamcastGenerator(info, item, 42, scope));
}

TEST_CASE("Dreamcast B1 admits all native troll generators and ranged variants",
          "[legends][dreamcast-enemies][assets]") {
    const auto root = nativeRoot();
    const auto level = root / "levels/levelb1";
    const auto native = formats::LegendsDreamcastScene::parse(readFile(level / "worlds.rom"));
    const auto scope =
        formats::dreamcastLegendsFixtureScope(readFile(root / "worlddata/mount.wad"), "levelb1");
    REQUIRE(scope);
    usize generators = 0, actors = 0;
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

TEST_CASE("Dreamcast native enemy archive renders brood and all ranged bodies",
          "[legends][dreamcast-enemies][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 32, {}, 1);
    std::vector<std::string> warnings;
    REQUIRE(enemies.setArchiveLoader(
        [&](s32 kind) { return loadLegendsDreamcastEnemyAssets(root, kind, warnings); }));
    REQUIRE(enemies.loadKind(enemy_type::kTroll));
    const auto* archive = enemies.archiveOf(enemy_type::kTroll);
    REQUIRE(archive);
    for (const auto tree :
         {"TRO1", "TRO2", "TRO3", "TROA", "TROB", "TROS", "TRO_ARROW", "TRO_BOMB"})
        CHECK(archive->trees.find(tree));
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 10}}};
    for (s32 tier = 1; tier <= 6; ++tier) {
        EnemySpawn spawn;
        spawn.kind = enemy_type::kTroll;
        spawn.tier = tier;
        spawn.position = {static_cast<f32>(tier) * 4, 0, 0};
        spawn.placed = true;
        REQUIRE(enemies.spawn(spawn, players));
    }
    enemies.update(1, 1.f / 30, players);
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    CHECK(warnings.empty());

    Generators generators;
    REQUIRE(generators.bind(device,
                            WorldLayout::fromDecoded({}, {}, {-100, -1, -100}, {100, 10, 100}),
                            enemies, nullptr, {}, 1));
    ImportedGenerator imported;
    imported.nativeIndex = 70;
    imported.kind = enemy_type::kTroll;
    imported.position = {0, 0, -10};
    imported.yaw = -glm::radians(90.f); // converted DC editor +90 degrees
    imported.tier = 2;
    imported.body.type = ItemInfo::kGenerator;
    imported.body.name = "TRO";
    imported.body.collisionType = 1;
    imported.body.radius = 2;
    imported.body.height = 5;
    imported.body.hitPoints = 10;
    imported.maximumBrood = 1;
    imported.spawnInterval = 1;
    imported.algorithm = 7;
    REQUIRE(generators.spawnImported(device, enemies, imported));
    CHECK(generators.healthOf(0) == 20);
    REQUIRE(generators.obstacles().size() == 1);
    CHECK(std::abs(generators.obstacles()[0].yaw - glm::radians(90.f)) < .00001f);
    const auto before = enemies.count();
    generators.update(2, enemies, players, {}, true);
    CHECK(enemies.count() == before);
    generators.update(2, enemies, players);
    CHECK(enemies.count() == before + 1);
    CHECK(generators.livingOf(0) == 1);
    generators.update(120, enemies, players);
    CHECK(generators.livingOf(0) == 1);
    imported.maximumBrood = -1;
    CHECK_FALSE(generators.spawnImported(device, enemies, imported));
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
    // SetItem8c0a8340 takes the initial visual state from placement+0x30.
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
    // HitItem8c0afaa0 changes state only at templateHP and 2*templateHP.
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
    constexpr s32 reported = 16777265;
    const auto object = scene->fixtureObject(reported);
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
    checkGeneratorArtwork(device, *scene, reported, *species, "GEN_TRO1L1", 28, 32);
}

TEST_CASE("Dreamcast generator artwork falls back when its optional species bank is absent",
          "[legends][dreamcast-enemies][dreamcast-generator-banks][assets]") {
    const auto original = nativeRoot();
    const auto root = test::scratchDirectory("dreamcast-generator-missing-species");
    // Copy only this map's native fixtures and generic fallback (under 2 MB),
    // deliberately excluding every monster, item, sound and GameCube archive.
    for (const auto name :
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
    constexpr std::array models{"GEN_TRO0L1", "GEN_TRO1L1", "GEN_TRO2L1", "GEN_TRO3L1"};
    constexpr std::array<usize, 4> triangles{34, 28, 28, 22};
    constexpr std::array<u32, 4> sizes{16, 16, 16, 32};
    for (s32 state = 3; state >= 0; --state) {
        CAPTURE(state);
        const auto index = static_cast<usize>(state);
        const std::array states{
            ImportedGeneratorState{full->nativeIndex, 0, state, full->position, state > 0}};
        scene->updateGenerators(states);
        checkGeneratorArtwork(device, *scene, selected, *fallback, models[index], triangles[index],
                              sizes[index]);
    }
}

TEST_CASE("Dreamcast generator libraries respect the selected native enemy roster",
          "[legends][dreamcast-enemies][dreamcast-generator-banks]") {
    const auto root = test::scratchDirectory("dreamcast-generator-roster");
    formats::LegendsDreamcastFixtureScope scope;
    std::vector<std::string> warnings;
    const auto rejected = [&] {
        warnings.clear();
        CHECK_FALSE(loadLegendsDreamcastGeneratorAssets(root, enemy_type::kTroll, scope, warnings));
        REQUIRE(warnings.size() == 1);
        // Reject the roster before looking for files: an existing unrelated
        // species bank must not silently become an active level's dependency.
        CHECK(warnings[0] == "Dreamcast generator library is not selected by this level: tro; "
                             "using fallback artwork");
    };
    rejected();
    scope.enemySounds = {{.kind = enemy_type::kKnight, .subtype = 2}};
    rejected();
    // LoadMonster8c090000 chooses TROaux for category4 and a numbered bank
    // above10. Neither selects the base TRO bank containing generator art.
    scope.enemySounds = {{.kind = enemy_type::kTroll, .subtype = 4}};
    rejected();
    scope.enemySounds[0].subtype = 11;
    rejected();
    scope.enemySounds.push_back({.kind = enemy_type::kTroll, .subtype = 2});
    warnings.clear();
    CHECK_FALSE(loadLegendsDreamcastGeneratorAssets(root, enemy_type::kTroll, scope, warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "Dreamcast generator library is unavailable: tro; using fallback artwork");
}

TEST_CASE("Dreamcast generator species libraries retain every shipped model's resident artwork",
          "[legends][dreamcast-enemies][dreamcast-generator-banks][assets]") {
    const auto root = nativeRoot();
    auto generic = loadLegendsDreamcastArchive(root / "generators");
    REQUIRE(generic);
    // Native monster objects.rom definitions contain GEN_* in these sixteen
    // banks. ICE/WRM/WOL/PLA do not contribute GEN_* definitions to this corpus.
    constexpr std::array kinds{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 18, 19};
    constexpr std::array banks{"sco", "tro", "dem", "rat", "gru", "kni", "sna", "sor",
                               "mum", "spi", "liz", "tre", "mag", "zom", "dog", "ske"};
    constexpr std::array<usize, 16> expectedModels{2, 4, 3, 2, 4, 4, 3, 4, 4, 2, 4, 4, 2, 4, 3, 4};
    usize comparedModels = 0;
    for (usize index = 0; index < kinds.size(); ++index) {
        const s32 kind = kinds[index];
        CAPTURE(kind, banks[index]);
        const auto directory = root / "monsters" / banks[index];
        const auto modelBytes = readFile(directory / "objects.rom");
        const auto nativeModels = formats::decodeDreamcastModelLibrary(modelBytes);
        const auto nativeTextures = formats::decodeLegendsDreamcastTextures(
            modelBytes, readFile(directory / "textures.rom"));
        formats::LegendsDreamcastFixtureScope scope;
        // The medium/large/small classes all select a base bank. Keep the
        // auxiliary row first to exercise native mixed-roster selection.
        scope.enemySounds = {{.kind = kind, .subtype = 4}, {.kind = kind, .subtype = 2}};
        std::vector<std::string> warnings;
        auto archive = loadLegendsDreamcastGeneratorAssets(root, kind, scope, warnings);
        REQUIRE(archive);
        CHECK(warnings.empty());
        const std::string prefix = "GEN_" + std::string{enemyKind(kind).prefix};
        usize models = 0, residentParts = 0, differentFromGeneric = 0;
        for (const auto& native : nativeModels) {
            if (!native.name.starts_with(prefix))
                continue;
            CAPTURE(native.name);
            const auto loaded = archive->models.find(native.name);
            REQUIRE(loaded);
            const auto& mesh = archive->models.mesh(*loaded);
            CHECK(mesh.triangleCount() == native.mesh.triangleCount());
            REQUIRE(mesh.parts.size() == native.mesh.parts.size());
            bool different = false;
            for (usize part = 0; part < mesh.parts.size(); ++part) {
                const u32 slot = native.mesh.parts[part].texture;
                REQUIRE(slot < nativeTextures.size());
                CHECK(mesh.parts[part].texture == slot);
                const auto& texture = nativeTextures[slot];
                const auto& entry = archive->textures.entry(slot);
                CHECK(entry.name == texture.name);
                CHECK(entry.external() == texture.external);
                CHECK(entry.width == texture.width);
                CHECK(entry.height == texture.height);
                if (texture.external) {
                    // DEM's LAVA deliberately belongs to its current level,
                    // not to a fabricated picture in the species archive.
                    CHECK(texture.image.pixels.empty());
                    CHECK(archive->textures.cachedImage(slot) == nullptr);
                    continue;
                }
                CHECK(archive->textures.image(slot).pixels == texture.image.pixels);
                CHECK(archive->textures.image(slot).width == texture.image.width);
                CHECK(archive->textures.image(slot).height == texture.image.height);
                if (!texture.image.pixels.empty()) {
                    ++residentParts;
                    if (const auto low = generic->textures.find(texture.name))
                        different |= generic->textures.image(*low).pixels != texture.image.pixels;
                }
            }
            differentFromGeneric += different ? 1 : 0;
            ++models;
        }
        CHECK(models == expectedModels[index]);
        CHECK(residentParts > 0);
        CHECK(differentFromGeneric > 0);
        comparedModels += models;
    }
    CHECK(comparedModels == 53);
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
    bool sawLava = false, sawResident = false;
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
            if (info.type != 3)
                continue;
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
            for (s32 id = 0; id < Enemies::kMost; ++id)
                if (enemies.alive(id) && !enemies.dying(id)) {
                    REQUIRE(child < 0);
                    child = id;
                }
            REQUIRE(child >= 0);
            CHECK(enemies.kindOf(child) == kinds[choice]);
            CHECK(enemies.tierOf(child) == tier);
            if (ways[choice] != 0)
                CHECK(enemies.algorithmOf(child) == ways[choice]);
            seen.insert(enemies.kindOf(child));
            device.draws.clear();
            enemies.draw(device, Mat4{1}, WorldLighting{});
            CHECK_FALSE(device.draws.empty());
            enemies.hurt(child, EnemyHit{.damage = 10000});
            enemies.update(240, 4, views);
        }
        CHECK(seen == std::set<s32>(kinds.begin(), kinds.end()));
        // A missing optional selected archive must fail closed, not load an
        // unrelated base model. The normal three-species path is tested below.
        CHECK_FALSE(loadLegendsDreamcastEnemyAssets(root / "absent", kinds.front(), warnings,
                                                    scope->enemySounds));
    }
}

TEST_CASE("Imported three-species brood retains its original seeded selection",
          "[legends][dreamcast-enemies][dreamcast-mixed-generators][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 32, {}, 1);
    std::vector<std::string> warnings;
    REQUIRE(enemies.setArchiveLoader(
        [&](s32 kind) { return loadLegendsDreamcastEnemyAssets(root, kind, warnings); }));
    Generators generators;
    REQUIRE(generators.bind(device,
                            WorldLayout::fromDecoded({}, {}, {-100, -1, -100}, {100, 10, 100}),
                            enemies, nullptr, {}, 1));
    ImportedGenerator definition;
    definition.kind = 2;
    definition.broodKinds = {2, 5, 13}; // unchanged N64 Temple policy
    definition.broodTier = 3;
    definition.body.type = ItemInfo::kGenerator;
    definition.body.name = "DEM";
    definition.body.collisionType = 1;
    definition.body.radius = 2;
    definition.body.height = 5;
    definition.body.hitPoints = 10;
    definition.spawnInterval = 1;
    definition.maximumBrood = 1;
    definition.alwaysActive = true;
    REQUIRE(generators.spawnImported(device, enemies, definition));
    const std::array views{EnemyView{.player = 0, .position = {0, 0, 15}}};
    Mt19937 expected(0x4c454745U);
    for (s32 birth = 0; birth < 24; ++birth) {
        generators.update(1000, enemies, views, {}, true);
        const usize choice = ((expected() >> 17U) * 3U) >> 15U;
        generators.update(2, enemies, views);
        s32 child = -1;
        for (s32 id = 0; id < Enemies::kMost; ++id)
            if (enemies.alive(id) && !enemies.dying(id)) {
                REQUIRE(child < 0);
                child = id;
            }
        REQUIRE(child >= 0);
        CHECK(enemies.kindOf(child) == definition.broodKinds[choice]);
        CHECK(enemies.tierOf(child) == 3);
        enemies.hurt(child, EnemyHit{.damage = 10000});
        enemies.update(240, 4, views);
    }
    CHECK(warnings.empty());
}

TEST_CASE("Dreamcast F1 breeds four enemies with the native thirty to ninety tick clock",
          "[legends][dreamcast-enemies][dreamcast-generator-cadence][assets]") {
    const auto root = nativeRoot();
    const auto decoded =
        formats::LegendsDreamcastScene::parse(readFile(root / "levels/levelf1/worlds.rom"));
    const auto scope =
        formats::dreamcastLegendsFixtureScope(readFile(root / "worlddata/hell.wad"), "levelf1");
    REQUIRE(scope);
    REQUIRE(scope->generatorDifficulty == 1);
    std::optional<LegendsDreamcastGenerator> native;
    for (usize index = 0; index < decoded.itemInstances.size() && !native; ++index) {
        const auto& item = decoded.itemInstances[index];
        native = legendsDreamcastGenerator(decoded.itemInfos[static_cast<usize>(item.info)], item,
                                           static_cast<u32>(index), *scope);
    }
    REQUIRE(native);
    auto definition = native->gameplay;
    // Isolate one real native generator in an unobstructed arena. Keep all
    // imported breeding, durability, species and strategy parameters intact.
    definition.position = {0, 0, 0};
    definition.minimumPlayers = 1;
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    Enemies enemies;
    enemies.open(device, root, nullptr, Enemies::kMost, {}, 1);
    REQUIRE(enemies.setArchiveLoader([&](s32 kind) {
        return loadLegendsDreamcastEnemyAssets(root, kind, warnings, scope->enemySounds);
    }));
    Generators generators;
    REQUIRE(generators.bind(device,
                            WorldLayout::fromDecoded({}, {}, {-100, -1, -100}, {100, 10, 100}),
                            enemies, nullptr, {}, 1));
    REQUIRE(generators.spawnImported(device, enemies, definition));
    REQUIRE(generators.mostOf(0) == 4);
    const std::array views{EnemyView{.player = 0, .position = {0, 0, 45}}};

    SECTION("concurrent brood reaches four and a full quota freezes its clock") {
        for (s32 tick = 0; tick < 1800 && generators.livingOf(0) < 4; ++tick) {
            generators.update(1, enemies, views);
            enemies.update(1, 1.f / 60, views);
        }
        REQUIRE(generators.livingOf(0) == 4);
        CHECK(generators.bredOf(0) == 4);
        const s32 held = generators.countdownOf(0);
        CHECK(held == 52);
        generators.update(600, enemies, views);
        CHECK(generators.livingOf(0) == 4);
        CHECK(generators.bredOf(0) == 4);
        CHECK(generators.countdownOf(0) == held);
    }
    SECTION("native waits expire on their final tick, wrap, and respect admission holds") {
        constexpr std::array expected{30, 37, 45, 52, 60, 67, 75, 82, 90, 30};
        for (usize birth = 0; birth < expected.size(); ++birth) {
            CAPTURE(birth);
            if (birth != 0) {
                const s32 remaining = generators.countdownOf(0);
                const std::array distant{EnemyView{.player = 0, .position = {0, 0, 51}}};
                generators.update(600, enemies, distant);
                CHECK(generators.countdownOf(0) == remaining);
                generators.update(600, enemies, views, {}, true);
                CHECK(generators.countdownOf(0) == remaining);
                generators.update(remaining - 1, enemies, views);
                CHECK(generators.bredOf(0) == static_cast<s32>(birth));
                CHECK(generators.countdownOf(0) == 1);
            }
            generators.update(1, enemies, views);
            REQUIRE(generators.bredOf(0) == static_cast<s32>(birth + 1));
            CHECK(generators.countdownOf(0) == expected[birth]);
            s32 child = -1;
            for (s32 id = 0; id < Enemies::kMost; ++id)
                if (enemies.alive(id) && !enemies.dying(id)) {
                    REQUIRE(child < 0);
                    child = id;
                }
            REQUIRE(child >= 0);
            CHECK(enemies.tierOf(child) == 3);
            enemies.hurt(child, EnemyHit{.damage = 10000});
            enemies.update(240, 4, views);
        }
    }
    SECTION("failed births retry without consuming cadence and replay exactly after rewind") {
        generators.update(1, enemies, views);
        REQUIRE(generators.bredOf(0) == 1);
        REQUIRE(generators.livingOf(0) == 1);
        REQUIRE(generators.countdownOf(0) == 30);
        generators.update(29, enemies, views);
        REQUIRE(generators.countdownOf(0) == 1);
        const f32 ratio = generators.slot(0).ratio;
        const std::array blockers{
            Obstacle{.centre = {0, 0, 0}, .halfAcross = 100, .halfAlong = 100, .height = 50}};
        Generators::LocalSnapshot beforeGenerators;
        Enemies::LocalSnapshot beforeEnemies;
        generators.saveLocal(beforeGenerators);
        REQUIRE(enemies.saveLocal(beforeEnemies));
        const auto correction = [&] {
            // The first three attempted births have no clear exit. This may
            // consume species/exit RNG, but not quota, successful births or
            // cadence. Removing the obstacle admits the very next tick.
            for (s32 tick = 0; tick < 8; ++tick) {
                CAPTURE(tick);
                if (tick < 3) {
                    generators.update(1, enemies, views, blockers);
                    REQUIRE(generators.bredOf(0) == 1);
                    CHECK(generators.livingOf(0) == 1);
                    CHECK(generators.countdownOf(0) == 0);
                    CHECK(generators.slot(0).ratio == ratio);
                } else {
                    generators.update(1, enemies, views);
                    REQUIRE(generators.bredOf(0) == 2);
                    CHECK(generators.livingOf(0) == 2);
                    CHECK(generators.countdownOf(0) == 37 - (tick - 3));
                }
                enemies.update(1, 1.f / 60, views);
            }
        };
        correction();
        Generators::LocalSnapshot expectedGenerators;
        Enemies::LocalSnapshot expectedEnemies;
        generators.saveLocal(expectedGenerators);
        REQUIRE(enemies.saveLocal(expectedEnemies));

        REQUIRE(generators.restoreLocal(beforeGenerators));
        REQUIRE(enemies.restoreLocal(beforeEnemies));
        // A speculative clear-exit branch births earlier. Discard both owners,
        // including the failed-attempt RNG and pending brood/animation events.
        for (s32 tick = 0; tick < 8; ++tick) {
            generators.update(1, enemies, views);
            enemies.update(1, 1.f / 60, views);
        }
        CHECK(generators.countdownOf(0) == 30);
        REQUIRE(generators.restoreLocal(beforeGenerators));
        REQUIRE(enemies.restoreLocal(beforeEnemies));
        correction();
        Generators::LocalSnapshot replayedGenerators;
        Enemies::LocalSnapshot replayedEnemies;
        generators.saveLocal(replayedGenerators);
        REQUIRE(enemies.saveLocal(replayedEnemies));
        CHECK(generators.equalLocal(expectedGenerators, replayedGenerators));
        CHECK(enemies.equalLocal(expectedEnemies, replayedEnemies));
    }
    SECTION("fallen or absent party members cannot advance the native birth clock") {
        generators.update(1, enemies, views);
        REQUIRE(generators.bredOf(0) == 1);
        REQUIRE(generators.countdownOf(0) == 30);
        std::array<PlayerRuntime, 1> players;
        players[0].actor.spawn(0, {}, nullptr, views[0].position, 0);
        for (const auto life : {PlayerLife::Dying, PlayerLife::InTower}) {
            players[0].life = life;
            const auto hidden = LevelOpponents::enemyViews(players);
            REQUIRE(hidden.size() == 1);
            REQUIRE(hidden[0].hidden);
            generators.update(600, enemies, hidden);
            CHECK(generators.bredOf(0) == 1);
            CHECK(generators.countdownOf(0) == 30);
        }
        generators.update(600, enemies, {});
        CHECK(generators.countdownOf(0) == 30);
        players[0].life = PlayerLife::Standing;
        generators.update(1, enemies, LevelOpponents::enemyViews(players));
        CHECK(generators.countdownOf(0) == 29);
    }
    CHECK(warnings.empty());
}

TEST_CASE("Dreamcast generator quota follows damage state and retained game difficulty",
          "[legends][dreamcast-enemies][dreamcast-generator-cadence][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    for (const std::string code : {"levelb1", "levele1", "levelf1"}) {
        const auto decoded =
            formats::LegendsDreamcastScene::parse(readFile(root / "levels" / code / "worlds.rom"));
        const auto metadata = formats::dreamcastLegendsWorldDataFile(code);
        REQUIRE_FALSE(metadata.empty());
        const auto scope = formats::dreamcastLegendsFixtureScope(readFile(root / metadata), code);
        REQUIRE(scope);
        std::optional<LegendsDreamcastGenerator> native;
        for (usize index = 0; index < decoded.itemInstances.size() && !native; ++index) {
            const auto& item = decoded.itemInstances[index];
            auto candidate =
                legendsDreamcastGenerator(decoded.itemInfos[static_cast<usize>(item.info)], item,
                                          static_cast<u32>(index), *scope);
            if (candidate && (code != "levelb1" || candidate->gameplay.tier == 3))
                native = std::move(candidate);
        }
        REQUIRE(native);
        for (const s32 party : {1, 4}) {
            for (const f32 gain : {.5f, 1.f, 2.f, 10.f}) {
                CAPTURE(code, party, gain);
                Enemies enemies;
                enemies.open(device, root, nullptr, Enemies::kMost, {}, party);
                REQUIRE(enemies.setArchiveLoader([&](s32 kind) {
                    return loadLegendsDreamcastEnemyAssets(root, kind, warnings,
                                                           scope->enemySounds);
                }));
                Generators generators;
                REQUIRE(generators.bind(
                    device, WorldLayout::fromDecoded({}, {}, {-100, -1, -100}, {100, 10, 100}),
                    enemies, nullptr, {}, party));
                auto definition = native->gameplay;
                definition.position = {0, 0, 0};
                definition.minimumPlayers = 1;
                REQUIRE(generators.spawnImported(device, enemies, definition, gain));
                std::vector<EnemyView> views;
                for (s32 seat = 0; seat < party; ++seat)
                    views.push_back(EnemyView{.player = seat, .position = {0, 0, 45}});
                generators.update(1, enemies, views);
                REQUIRE(generators.bredOf(0) == 1);
                const f32 difficulty = scope->generatorDifficulty * gain;
                CHECK(generators.countdownOf(0) ==
                      static_cast<s32>(30 / std::clamp(difficulty, .5f, 2.f)));
                const f32 bonus = code == "levele1" ? .5f : code == "levelf1" ? 1.f : 0.f;
                constexpr std::array nativeBase{6, 4, 2};
                for (s32 state = definition.tier; state > 0; --state) {
                    REQUIRE(generators.stateOf(0) == state);
                    const s32 quota =
                        std::clamp(static_cast<s32>(
                                       static_cast<f32>(nativeBase[static_cast<usize>(state - 1)]) *
                                           (std::clamp(difficulty, .25f, 3.f) + bonus) +
                                       .5f),
                                   1, 10);
                    CHECK(generators.mostOf(0) == quota);
                    REQUIRE(generators.strike(
                        0, static_cast<f32>(definition.body.hitPoints + definition.body.armor), 0));
                }
                CHECK_FALSE(generators.standing(0));
            }
        }
    }
    CHECK(warnings.empty());
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
            for (s32 id = 0; id < Enemies::kMost; ++id)
                if (opponents.enemies().alive(id) && opponents.enemies().generatorOf(id) >= 0) {
                    child = id;
                    break;
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
