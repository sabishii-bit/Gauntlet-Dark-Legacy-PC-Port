#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/n64/scenes/LegendsSumnerText.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::string words(std::string_view value) {
    std::istringstream input{std::string{value}};
    std::string result;
    std::string word;
    while (input >> word) {
        if (!result.empty()) {
            result += ' ';
        }
        result += word;
    }
    return result;
}

struct SumnerScene {
    std::filesystem::path root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    std::filesystem::path directory = root / "levels/tower2";
    std::filesystem::path characters;
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    formats::LegendsN64Tower tower;
    std::shared_ptr<LegendsLevel> level;
    LevelWorld world;
    GameConfig config;
    StringTable strings;
    AssetLocator assets;
    PlayScene scene;

    SumnerScene()
        : characters(test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path()),
          assets(characters) {
        if (!std::filesystem::is_regular_file(directory / "worlds.wad") ||
            !std::filesystem::is_regular_file(root / "_program/Tower.bin")) {
            SKIP("Native N64 Tower extraction is not installed");
        }
        tower = formats::decodeLegendsN64Tower(readFile(root / "_program/Tower.bin"));
        const auto objects = readFile(directory / "objects.wad");
        map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                            readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
        REQUIRE(adapter->isTower());
        REQUIRE(adapter->cameraConfig());
        level =
            std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, characters);
        ClassDataSet classes;
        REQUIRE(classes.load(characters / "pdata"));
        CharacterSave save;
        save.progress().health = 500;
        save.autoAttack = false;
        CharacterSave second = save;
        second.color = 1;
        const std::array party{PartyMember{0, save}, PartyMember{1, second}};
        level->initializeParty(party);
        LegendsTraversal traversal;
        traversal.open(map, 0, save, classes.stats(save.character));
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "n64:tower2";
        REQUIRE(world.loadImported(device, characters, ref, std::move(layout),
                                   traversal.collision(), level));
        config.camera.compass = false;
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        GameContext context;
        context.config = &config;
        context.strings = &strings;
        context.assets = &assets;
        context.unpackedRoot = characters;
        PlayOptions options;
        options.welcome = false;
        // Place the test party on the real Tower floor, south of the authored
        // podium. Only the approach is shortened; all collision and movement
        // still flow through PlayScene, PartyMotion and native scenery contact.
        const auto podium = tower.residentVolumes[1].centre;
        options.position = Vec3{podium.x - PlayScene::kSpawnSpacing * .5f, 0, podium.z - 6};
        options.yaw = 0;
        REQUIRE(scene.open(device, context, world, party, options));
        const auto entering = [&] {
            return scene.animator(0)->entering() || scene.animator(1)->entering();
        };
        REQUIRE(scene.animator(0));
        REQUIRE(scene.animator(1));
        // The native camera can finish before the ordinary player START clips.
        // PlayScene correctly holds imported callbacks until both clocks finish.
        for (u32 tick = 0;
             tick < 1800 && (scene.spawning() || !level->opponentsReady() || entering()); ++tick) {
            step();
        }
        REQUIRE_FALSE(scene.spawning());
        REQUIRE(level->opponentsReady());
        REQUIRE_FALSE(entering());
        REQUIRE_FALSE(scene.hints().active());
        REQUIRE_FALSE(scene.scroll().active());
        REQUIRE(scene.actorCount() == 2);
    }

    ~SumnerScene() { scene.close(); }

    void step(const PlayScene::Inputs& inputs = {}) {
        REQUIRE(scene.update(1.0 / 60, inputs) == PlayOutcome::Running);
    }

    void approach() {
        REQUIRE_FALSE(scene.hints().active());
        for (u32 tick = 0; tick < 600 && !scene.hints().active(); ++tick) {
            const Vec3 delta = tower.residentVolumes[1].centre - scene.actor(1)->position();
            const f32 heading = std::atan2(delta.x, delta.z) - scene.simulationCamera().yaw;
            PlayScene::Inputs inputs{};
            inputs[1].move = {{std::sin(heading), std::cos(heading)}, .65f};
            step(inputs);
        }
        INFO("Visitor position " << scene.actor(1)->position().x << ", "
                                 << scene.actor(1)->position().y << ", "
                                 << scene.actor(1)->position().z);
        REQUIRE(scene.hints().active());
        CHECK_FALSE(scene.canPause(0));
        CHECK_FALSE(scene.canPause(1));
        for (u32 tick = 0; tick < 90; ++tick) {
            step();
        }
    }

    void choose(usize row, s32 seat = 1) {
        const auto area = scene.hints().topics().itemArea(row);
        PlayScene::Inputs input{};
        input[static_cast<usize>(seat)].menu.pointer =
            Vec2{area.x + area.width * .5f, area.y + area.height * .5f};
        input[static_cast<usize>(seat)].menu.pointerPressed = true;
        step(input);
    }

    std::string readPages() {
        REQUIRE(scene.scroll().active());
        std::string prose;
        usize previousPage = static_cast<usize>(-1);
        for (u32 tick = 0; tick < 1800 && scene.scroll().active(); ++tick) {
            if (!scene.scroll().burning() && scene.scroll().page() != previousPage) {
                previousPage = scene.scroll().page();
                for (const auto& line : scene.scroll().lines()) {
                    prose += line + ' ';
                }
            }
            PlayScene::Inputs input{};
            input[1].menu.select = true;
            step(input);
        }
        REQUIRE_FALSE(scene.scroll().active());
        REQUIRE_FALSE(scene.hints().active());
        return words(prose);
    }

    void checkMenuArt() {
        TextureSet images;
        REQUIRE(images.load(characters / "STATIC"));
        const auto backdrop = images.find("SCROLL_A");
        const auto ink = images.find("FONT32_PARCH");
        REQUIRE(backdrop);
        REQUIRE(ink);
        device.draws.clear();
        REQUIRE(device.beginFrame());
        scene.render(device, Mat4{1}, 640, 448);
        device.endFrame();
        const auto drawn = [&](const Image& image) {
            return std::ranges::any_of(device.draws, [&](const auto& draw) {
                const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
                return texture && !draw.vertices.empty() && texture->width() == image.width &&
                       texture->height() == image.height && texture->pixels == image.pixels;
            });
        };
        CHECK(drawn(images.image(*backdrop)));
        CHECK(drawn(images.image(*ink)));
    }
};
} // namespace

TEST_CASE("Native Tower podium contact routes the complete Sumner menu to its visitor",
          "[legends][sumner-integration][imported-play][assets]") {
    SumnerScene fixture;
    const Vec3 start = fixture.scene.actor(1)->position();
    fixture.approach();
    CHECK(glm::distance(start, fixture.scene.actor(1)->position()) > 1.0f);
    const auto& menu = fixture.scene.hints().topics();
    const std::array<s32, 6> actions{0x11, 0x15, 0x2e, 0xd, 0x2f, 2};
    REQUIRE(menu.definition().items.size() == actions.size());
    CHECK(menu.definition().title == fixture.strings.get(legendsN64SumnerTextKey(441)));
    for (usize i = 0; i < actions.size(); ++i) {
        CHECK(menu.definition().items[i].code == actions[i]);
    }
    fixture.checkMenuArt();

    // The first local player cannot operate the second player's native menu.
    const auto selection = menu.selection();
    fixture.choose(2, 0);
    REQUIRE(fixture.scene.hints().active());
    CHECK(menu.selection() == selection);
    PlayScene::Inputs wrong{};
    wrong[0].menu.escape = true;
    wrong[0].menu.down = true;
    wrong[0].menu.select = true;
    fixture.step(wrong);
    REQUIRE(fixture.scene.hints().active());
    CHECK(menu.selection() == selection);
    fixture.choose(2);
    CHECK_FALSE(fixture.scene.hints().active());
    CHECK(fixture.readPages() == words(fixture.strings.get(legendsN64SumnerTextKey(500))));

    fixture.approach();
    fixture.choose(0); // Native Continue: return to gameplay, not the GDL hint-page menu.
    CHECK_FALSE(fixture.scene.hints().active());
    CHECK_FALSE(fixture.scene.scroll().active());
    CHECK(fixture.scene.canPause(1));

    fixture.approach();
    fixture.choose(5); // Sixth row must not be lost to the four-topic online input encoding.
    CHECK(fixture.readPages() == words(fixture.strings.get(legendsN64SumnerHistoryKey(1))));
    fixture.approach();
    PlayScene::Inputs escape{};
    escape[1].menu.escape = true;
    fixture.step(escape);
    for (u32 tick = 0; tick < 300 && fixture.scene.hints().active(); ++tick) {
        fixture.step();
    }
    CHECK_FALSE(fixture.scene.hints().active());
    CHECK(fixture.scene.canPause(1));
}

TEST_CASE("Native Tower return presents new milestones once and restores the matching History",
          "[legends][sumner-integration][native-progress-flow][assets]") {
    SumnerScene fixture;
    const auto before = fixture.scene.party();
    auto* save = fixture.scene.liveSave(1);
    REQUIRE(save);
    auto& native = save->progress().legendsProgress(LegendsPlatform::N64);
    native.obelisks = 7;
    native.runestones = 1;
    native.shards = 1;
    REQUIRE(native.recordCompleted("mount6"));
    // Pass through the real save serialization boundary before the Tower sees
    // these pieces. Neither the GDL nor Dreamcast campaign authorizes this gate.
    *save = CharacterSave::fromJson(save->toJson());
    const auto returned = fixture.scene.party();
    fixture.level->announceProgress(before, returned);
    fixture.step();
    REQUIRE(fixture.scene.scroll().active());
    std::string expected;
    for (const u32 id : {541U, 523U, 524U, 531U, 532U}) {
        expected += fixture.strings.get(legendsN64SumnerTextKey(id));
        expected += ' ';
    }
    CHECK(fixture.readPages() == words(expected));
    for (u32 i = 0; i < 90; ++i)
        fixture.step();
    CHECK_FALSE(fixture.scene.scroll().active());
    fixture.level->announceProgress(returned, returned);
    fixture.step();
    CHECK_FALSE(fixture.scene.scroll().active());
    fixture.approach();
    fixture.choose(5);
    CHECK(fixture.readPages() == words(fixture.strings.get(legendsN64SumnerHistoryKey(12))));
}
