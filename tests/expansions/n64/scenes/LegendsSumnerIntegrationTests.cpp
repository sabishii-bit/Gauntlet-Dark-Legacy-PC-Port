#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/SpecialMembers.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
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
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;
    f32 arrivalYaw = 0; ///< a Tower return's heading at its start, before any turn

    /** The Tower settled into play, the party south of Sumner's podium. With `brought` it is
     * a Tower return as the app opens one instead: the second member comes home with those
     * pieces, their ceremony is queued before the scene opens, and the party stands at the
     * Tower's start with its native heading, as the ceremony opens the visit. */
    explicit SumnerScene(std::optional<LegendsProgress> brought = std::nullopt)
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
        std::array party{PartyMember{0, save}, PartyMember{1, second}};
        const auto left = party;
        if (brought) {
            auto& native = party[1].save.progress().legendsProgress(LegendsPlatform::N64);
            native.runestones |= brought->runestones;
            native.shards |= brought->shards;
            native.obelisks |= brought->obelisks;
        }
        level->initializeParty(party);
        if (brought) {
            level->announceProgress(left, party);
        }
        LegendsTraversal traversal;
        traversal.open(map, 0, save, classes.stats(save.character));
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "n64:tower2";
        const bool loaded = world.loadImported(device, characters, ref, std::move(layout),
                                               traversal.collision(), level);
        REQUIRE(loaded);
        config.camera.compass = false;
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        context.config = &config;
        context.strings = &strings;
        context.assets = &assets;
        context.unpackedRoot = characters;
        PlayOptions options;
        options.welcome = false;
        if (brought) {
            options.position = traversal.start();
            options.yaw = arrivalYaw = traversal.actor().yaw();
            REQUIRE(scene.open(device, context, world, party, options));
            return; // the ceremony holds play until it hands back
        }
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

    GDL_NON_COPYABLE_NON_MOVABLE(SumnerScene);
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
        auto previousPage = static_cast<usize>(-1);
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
        const auto images = formats::decodeLegendsN64Textures(readFile(root / "hud/objects.wad"));
        const auto backdrop =
            std::ranges::find(images, std::string{"TEXTSCROLL"}, &formats::LegendsN64Texture::name);
        REQUIRE(backdrop != images.end());
        const auto body = formats::LegendsN64Font::parse(readFile(root / "Font/glfm3.fnt"));
        const auto heading = formats::LegendsN64Font::parse(readFile(root / "Font/bigfont.fnt"));
        device.draws.clear();
        REQUIRE(device.beginFrame());
        scene.render(device, Mat4{1}, 640, 448);
        device.endFrame();
        const auto drawn = [&](const Image& image) -> const test::RecordedDraw* {
            const auto found = std::ranges::find_if(device.draws, [&](const auto& draw) {
                const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
                return texture && !draw.vertices.empty() && texture->width() == image.width &&
                       texture->height() == image.height && texture->pixels == image.pixels;
            });
            return found == device.draws.end() ? nullptr : &*found;
        };
        const auto* scrollDraw = drawn(backdrop->image);
        const auto* bodyDraw = drawn(body.atlas);
        const auto* headingDraw = drawn(heading.atlas);
        REQUIRE(scrollDraw);
        REQUIRE(bodyDraw);
        REQUIRE(headingDraw);
        CHECK(scrollDraw->vertices.size() == 6);
        CHECK(bodyDraw->vertices.front().color == Color::rgba(0, 132, 0));

        // The native header uses font8, not OptionMenu's font5 body painter.
        // Verify the actual visible "Sumner" glyphs, their UVs, size and colour,
        // rather than accepting an empty generic title field as sufficient.
        test::FakeRenderDevice expected;
        test::FakeTexture sheet{heading.atlas.width, heading.atlas.height};
        TextPainter text;
        text.setFont(&heading.font, &sheet);
        Canvas canvas;
        canvas.begin(expected, Mat4{1});
        constexpr f32 kNativeScale = 512.0f / 416.0f;
        const auto area = scene.hints().topics().backdropArea();
        text.draw(canvas, -256, static_cast<s32>(area.y + 10 * kNativeScale),
                  strings.get(legendsN64SumnerTextKey(441)),
                  {.scale = 1.5f * kNativeScale, .color = Color::rgba(0, 0, 132)});
        canvas.end();
        REQUIRE(expected.draws.size() == 1);
        CHECK(headingDraw->vertices == expected.draws.front().vertices);
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
    const std::array<u32, 6> labels{426, 430, 442, 443, 444, 445};
    REQUIRE(menu.definition().items.size() == actions.size());
    CHECK(menu.definition().title.empty()); // The distinct native title font is checked below.
    for (usize i = 0; i < actions.size(); ++i) {
        CHECK(menu.definition().items[i].code == actions[i]);
        CHECK(menu.definition().items[i].text ==
              fixture.strings.get(legendsN64SumnerTextKey(labels[i])));
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
    const auto* ceremony = fixture.level->adapter().towerCeremony();
    REQUIRE(ceremony);
    REQUIRE(ceremony->active());
    CHECK_FALSE(fixture.scene.scroll().active());
    CHECK_FALSE(fixture.level->opponentsReady());
    CHECK(ceremony->displayedProgress().runestones == 0);
    CHECK(ceremony->displayedProgress().shards == 0);
    std::vector<std::string> gestures;
    std::vector<u32> captions;
    bool captionDrawn = false;
    const Vec3 stationary = fixture.scene.actor(1)->position();
    for (u32 i = 0; i < 6000 && ceremony->active(); ++i) {
        const std::string gesture{ceremony->gesture()};
        if (std::ranges::find(gestures, gesture) == gestures.end()) {
            gestures.push_back(gesture);
        }
        if (const auto text = ceremony->caption();
            text && std::ranges::find(captions, *text) == captions.end()) {
            captions.push_back(*text);
        }
        if (!captionDrawn && ceremony->caption() == 541U && ceremony->captionOpacity() >= 1) {
            // The level draws the scene's string table line, not the disc's bank.
            captionDrawn = true;
            const auto* style = fixture.level->messagePresentation();
            REQUIRE(style);
            Canvas canvas;
            fixture.device.draws.clear();
            canvas.begin(fixture.device, Mat4{1});
            fixture.level->drawEntranceTitle(canvas, 512);
            canvas.end();
            REQUIRE(fixture.device.draws.size() == 1);
            const auto drawn = fixture.device.draws.front().vertices;
            fixture.device.draws.clear();
            canvas.begin(fixture.device, Mat4{1});
            style->text().draw(canvas, -256, static_cast<s32>(30 * style->scale()),
                               fixture.strings.get(legendsN64CeremonyCaptionKey(541)),
                               {.scale = style->scale(), .color = Color::white()});
            canvas.end();
            REQUIRE(fixture.device.draws.size() == 1);
            CHECK(fixture.device.draws.front().vertices == drawn);
            fixture.device.draws.clear();
        }
        PlayScene::Inputs attemptedMovement{};
        attemptedMovement[1].move = {{1, 0}, 1};
        attemptedMovement[1].attackPressed = true;
        fixture.step(attemptedMovement);
        if (ceremony->active()) {
            CHECK(fixture.scene.actor(1)->position() == stationary);
        }
    }
    CHECK_FALSE(ceremony->active());
    CHECK(fixture.level->opponentsReady());
    CHECK(ceremony->displayedProgress().runestones == 1);
    CHECK(ceremony->displayedProgress().shards == 1);
    CHECK(ceremony->displayedProgress().obelisks == 7);
    CHECK(captions == std::vector<u32>{541, 523, 524, 531, 532});
    CHECK(captionDrawn);
    for (const auto* const gesture : {"MKRU", "STRT", "ODOR", "SHD1", "SHDR"}) {
        CHECK(std::ranges::find(gestures, gesture) != gestures.end());
    }
    CHECK_FALSE(fixture.scene.scroll().active());
    fixture.level->announceProgress(returned, returned);
    fixture.step();
    CHECK_FALSE(fixture.scene.scroll().active());
    CHECK_FALSE(ceremony->pending());
    fixture.approach();
    fixture.choose(5);
    CHECK(fixture.readPages() == words(fixture.strings.get(legendsN64SumnerHistoryKey(12))));
}

TEST_CASE("A Tower return opens on its ceremony with the party standing, turned to face Sumner",
          "[legends][sumner-integration][tower-ceremony][assets]") {
    LegendsProgress brought;
    brought.runestones = 1;
    SumnerScene fixture{brought};
    const auto* ceremony = fixture.level->adapter().towerCeremony();
    REQUIRE(ceremony);
    REQUIRE(ceremony->pending());
    REQUIRE_FALSE(ceremony->active());
    // Setup e08694e4 opens the visit in state 0x12, whose arrival (game.bin e004b070) stands
    // the party without e004b098's entrance effect. The arrival hold itself still runs.
    CHECK(fixture.scene.spawning());
    CHECK(fixture.scene.spawnEffectCount() == 0);
    // e0860198 stands Sumner at e086a678; the cut (e0860620) turns each arrived player to
    // e0010e50's arc tangent of his x and z offsets, through e0048acc. Sumner keeps his yaw.
    const auto overlay = readFile(fixture.root / "_program/Tower.bin");
    const auto coordinate = [&](usize at) { return std::bit_cast<f32>(readU32BE(overlay, at)); };
    const Vec3 sumner{coordinate(0xa678), coordinate(0xa67c), coordinate(0xa680)};
    const auto checkFacing = [&] {
        for (s32 seat = 0; seat < 2; ++seat) {
            const auto* actor = fixture.scene.actor(seat);
            REQUIRE(actor);
            const Vec3 offset = sumner - actor->position();
            CAPTURE(seat, fixture.arrivalYaw);
            CHECK(actor->yaw() == portable::atan2(offset.x, offset.z));
        }
    };
    checkFacing();
    // The first update cuts to his shot, the party held still as it was turned.
    fixture.step();
    REQUIRE(ceremony->active());
    checkFacing();
}
