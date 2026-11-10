#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/math/Mt19937.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/enemies/LegendsDreamcastEnemies.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/enemies/LevelOpponents.h"

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
    // SetItem overwrites byte 4 and does not read byte 6 for generators;
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
    for (const auto* const tree :
         {"TRO1", "TRO2", "TRO3", "TROA", "TROB", "TROS", "TRO_ARROW", "TRO_BOMB"}) {
        CHECK(archive->trees.find(tree));
    }
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
    scope.enemySounds = {{.kind = enemy_type::kKnight, .subtype = 2, .stream = {}}};
    rejected();
    // LoadMonster 8c090000 chooses TROaux for category 4 and a numbered bank
    // above 10. Neither selects the base TRO bank containing generator art.
    scope.enemySounds = {{.kind = enemy_type::kTroll, .subtype = 4, .stream = {}}};
    rejected();
    scope.enemySounds[0].subtype = 11;
    rejected();
    scope.enemySounds.push_back({.kind = enemy_type::kTroll, .subtype = 2, .stream = {}});
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
    constexpr std::array kKinds{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 18, 19};
    constexpr std::array kBanks{"sco", "tro", "dem", "rat", "gru", "kni", "sna", "sor",
                                "mum", "spi", "liz", "tre", "mag", "zom", "dog", "ske"};
    constexpr std::array<usize, 16> kExpectedModels{2, 4, 3, 2, 4, 4, 3, 4, 4, 2, 4, 4, 2, 4, 3, 4};
    usize comparedModels = 0;
    for (usize index = 0; index < kKinds.size(); ++index) {
        const s32 kind = kKinds[index];
        CAPTURE(kind, kBanks[index]);
        const auto directory = root / "monsters" / kBanks[index];
        const auto modelBytes = readFile(directory / "objects.rom");
        const auto nativeModels = formats::decodeDreamcastModelLibrary(modelBytes);
        const auto nativeTextures = formats::decodeLegendsDreamcastTextures(
            modelBytes, readFile(directory / "textures.rom"));
        formats::LegendsDreamcastFixtureScope scope;
        // The medium/large/small classes all select a base bank. Keep the
        // auxiliary row first to exercise native mixed-roster selection.
        scope.enemySounds = {{.kind = kind, .subtype = 4, .stream = {}},
                             {.kind = kind, .subtype = 2, .stream = {}}};
        std::vector<std::string> warnings;
        auto archive = loadLegendsDreamcastGeneratorAssets(root, kind, scope, warnings);
        REQUIRE(archive);
        CHECK(warnings.empty());
        const std::string prefix = "GEN_" + std::string{enemyKind(kind).prefix};
        usize models = 0;
        usize residentParts = 0;
        usize differentFromGeneric = 0;
        for (const auto& native : nativeModels) {
            if (!native.name.starts_with(prefix)) {
                continue;
            }
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
                    if (const auto low = generic->textures.find(texture.name)) {
                        different |= generic->textures.image(*low).pixels != texture.image.pixels;
                    }
                }
            }
            differentFromGeneric += different ? 1 : 0;
            ++models;
        }
        CHECK(models == kExpectedModels[index]);
        CHECK(residentParts > 0);
        CHECK(differentFromGeneric > 0);
        comparedModels += models;
    }
    CHECK(comparedModels == 53);
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
        for (s32 id = 0; id < Enemies::kMost; ++id) {
            if (enemies.alive(id) && !enemies.dying(id)) {
                REQUIRE(child < 0);
                child = id;
            }
        }
        REQUIRE(child >= 0);
        CHECK(enemies.kindOf(child) == definition.broodKinds[choice]);
        CHECK(enemies.tierOf(child) == 3);
        enemies.hurt(child, EnemyHit{.damage = 10000, .where = std::nullopt});
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
        constexpr std::array kExpected{30, 37, 45, 52, 60, 67, 75, 82, 90, 30};
        for (usize birth = 0; birth < kExpected.size(); ++birth) {
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
            CHECK(generators.countdownOf(0) == kExpected[birth]);
            s32 child = -1;
            for (s32 id = 0; id < Enemies::kMost; ++id) {
                if (enemies.alive(id) && !enemies.dying(id)) {
                    REQUIRE(child < 0);
                    child = id;
                }
            }
            REQUIRE(child >= 0);
            CHECK(enemies.tierOf(child) == 3);
            enemies.hurt(child, EnemyHit{.damage = 10000, .where = std::nullopt});
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
            if (candidate && (code != "levelb1" || candidate->gameplay.tier == 3)) {
                native = std::move(candidate);
            }
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
                views.reserve(static_cast<usize>(party));
                for (s32 seat = 0; seat < party; ++seat) {
                    views.push_back(EnemyView{.player = seat, .position = {0, 0, 45}});
                }
                generators.update(1, enemies, views);
                REQUIRE(generators.bredOf(0) == 1);
                const f32 difficulty = scope->generatorDifficulty * gain;
                CHECK(generators.countdownOf(0) ==
                      static_cast<s32>(30 / std::clamp(difficulty, .5f, 2.f)));
                const f32 bonus = code == "levele1" ? .5f : code == "levelf1" ? 1.f : 0.f;
                constexpr std::array kNativeBase{6, 4, 2};
                for (s32 state = definition.tier; state > 0; --state) {
                    REQUIRE(generators.stateOf(0) == state);
                    // The nearest whole brood, a half rounding up: no product here is negative.
                    const s32 quota = std::clamp(
                        static_cast<s32>(std::lround(
                            static_cast<f32>(kNativeBase[static_cast<usize>(state - 1)]) *
                            (std::clamp(difficulty, .25f, 3.f) + bonus))),
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
} // namespace
