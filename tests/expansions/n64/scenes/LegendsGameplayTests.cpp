#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/critters/LegendsCritters.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/app/ImportedShop.h"
#include "game/characters/ShopPurchase.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/level/ImportedLevel.h"
#include "game/party/TargetAssist.h"
#include "game/players/PowerupEffects.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

class PlainImportedLevel final : public ImportedLevel {
public:
    s32 captures = 0;
    s32 follows = 0;
    s32 partyUpdates = 0;
    usize partySize = 0;
    WorldCamera view{{-30, 30, 0}, kPi / 4, kPi / 2, 0};
    std::optional<WorldCamera> sampledView;
    mutable std::vector<f32> cameraSamples;
    mutable std::vector<WorldCamera> drawnCameras;
    std::optional<ImportedObjectHit> picked;
    mutable s32 highlighted = -1;

    void capturePresentation() override { ++captures; }
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& camera,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {
        drawnCameras.push_back(camera);
    }
    void followCamera(std::span<const CameraSubject> /*subjects*/, f32 /*seconds*/) override {
        ++follows;
    }
    std::optional<WorldCamera> camera(f32 alpha = -1) const override {
        cameraSamples.push_back(alpha);
        return alpha >= 0 && sampledView ? *sampledView : view;
    }
    std::optional<ImportedObjectHit> pickObject(const Vec3&, const Vec3&, const WorldCamera&,
                                                f32) const override {
        return picked;
    }
    void highlightObject(RenderDevice&, const Mat4&, const WorldCamera&, f32, s32 id,
                         const SelectionHighlight&) const override {
        highlighted = id;
    }
    void updateParty(std::span<PlayerActor*> players, f32 /*seconds*/,
                     bool /*timeStopped*/) override {
        ++partyUpdates;
        partySize = players.size();
    }
};

struct GameplayFixture {
    std::filesystem::path root = test::assetOrSkip("PLAYERS").parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    PlayScene scene;

    GameplayFixture() {
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        config.camera.compass = false;
        config.combat.autoMelee = false;
        config.combat.autoActivateItems = false;
    }

    void open(std::shared_ptr<ImportedLevel> imported, WorldCollision collision, const Vec3& start,
              f32 yaw, std::span<const PartyMember> party, Vec3 minimum = Vec3{-100},
              Vec3 maximum = Vec3{100}, bool finishEntrance = true) {
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = start}}, minimum, maximum);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "native-visit";
        ref.title = "Native visit";
        const bool loaded = world.loadImported(device, root, ref, std::move(layout),
                                               std::move(collision), std::move(imported));
        REQUIRE(loaded);
        GameContext context;
        context.config = &config;
        context.strings = &strings;
        context.tower = &world;
        context.unpackedRoot = root;
        PlayOptions options;
        options.position = start;
        options.yaw = yaw;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        if (!finishEntrance) {
            return;
        }
        for (s32 tick = 0; tick < 600 && entering(); ++tick) {
            step();
        }
        REQUIRE_FALSE(entering());
        REQUIRE_FALSE(scene.cinematicBars());
        REQUIRE(scene.canPause(party.front().player));
    }

    bool entering() const {
        return scene.spawning() ||
               std::ranges::any_of(scene.participants(), [](const auto& participant) {
                   return participant.figure && participant.figure->animator().entering();
               });
    }

    void step(const PlayScene::Inputs& input = {}) {
        REQUIRE(scene.update(1.0 / 60, input) == PlayOutcome::Running);
    }

    void fullHud() {
        device.draws.clear();
        scene.render(device, Mat4{1}, 640, 448);
        REQUIRE_FALSE(device.draws.empty());
        const auto projection =
            makeVirtualScreenTransform(Mat4{1}, static_cast<f32>(config.display.virtualWidth),
                                       static_cast<f32>(config.display.virtualHeight), 640, 448);
        std::vector<ImmediateVertex> overlay;
        for (const auto& draw : device.draws) {
            if (draw.transform == projection) {
                overlay.insert(overlay.end(), draw.vertices.begin(), draw.vertices.end());
            }
        }
        test::FakeRenderDevice expectedDevice;
        PartyHud expected;
        REQUIRE(expected.load(expectedDevice, root, &strings));
        if (scene.hud().relicsShown()) {
            expected.showRelics();
        }
        Canvas canvas;
        canvas.begin(expectedDevice, projection);
        expected.drawStatus(canvas, scene.participants());
        canvas.end();
        std::vector<ImmediateVertex> expectedVertices;
        std::array<bool, 4> plates{};
        for (const auto& draw : expectedDevice.draws) {
            expectedVertices.insert(expectedVertices.end(), draw.vertices.begin(),
                                    draw.vertices.end());
            for (const auto& vertex : draw.vertices) {
                if (vertex.position.y >= StatusBoxPainter::kY && vertex.position.x >= 0 &&
                    vertex.position.x < 4 * StatusBoxPainter::kWidth) {
                    plates[static_cast<usize>(vertex.position.x / StatusBoxPainter::kWidth)] = true;
                }
            }
        }
        REQUIRE_FALSE(expectedVertices.empty());
        CHECK(std::ranges::all_of(plates, [](bool drawn) { return drawn; }));
        // Compare the real localized four-plate painter, not a hard-coded draw count.
        // A one-plate preview or a null StringTable changes this vertex sequence.
        CHECK(std::search(overlay.begin(), overlay.end(), expectedVertices.begin(),
                          expectedVertices.end()) != overlay.end());
        REQUIRE(strings.get("select.levelShort") != "select.levelShort");
    }
};

WorldCollision plainFloor() {
    WorldCollision world;
    CollisionTriangle first;
    first.vertices = {{{-100, 0, -100}, {-100, 0, 100}, {100, 0, 100}}};
    first.objectFlags = 4;
    CollisionTriangle second;
    second.vertices = {{{-100, 0, -100}, {100, 0, 100}, {100, 0, -100}}};
    second.objectFlags = 4;
    world.build({first, second});
    return world;
}

PartyMember member(s32 player) {
    PartyMember result;
    result.player = player;
    result.slot = static_cast<usize>(player);
    result.save.name = "VISIT" + std::to_string(player);
    result.save.color = player;
    result.save.progress().health = 500;
    result.save.gold = 100;
    result.turbo = TurboMeter::kFull;
    return result;
}

TEST_CASE("Imported shop transactions resume the same party camera and encounter",
          "[legends][gameplay][imported-shop][assets]") {
    GameplayFixture fixture;
    auto imported = std::make_shared<PlainImportedLevel>();
    const std::array party{member(0), member(2)};
    fixture.open(imported, plainFloor(), {10, 0, 20}, .75f, party);
    auto& scene = fixture.scene;
    const auto before = scene.party();
    auto after = before;
    const ShopItem key{.type = 1, .price = 10, .amount = 1};
    REQUIRE(buyShopItem(after[0].save, ClassStats{}, key, 0) == ShopResult::Bought);
    const auto position = scene.actor(0)->position();
    const auto secondPosition = scene.actor(2)->position();
    const auto camera = scene.simulationCamera();
    const auto follows = imported->follows;
    const auto checkpoint = scene.participants()[0].resultsCheckpoint;
    bool accepted = true;
    SECTION("purchased inventory commits without another entry") {}
    SECTION("duplicate returned seats are atomic failures") {
        after[1].player = after[0].player;
        accepted = false;
    }
    SECTION("character changes cannot replace a parked actor") {
        ++after[1].save.character;
        accepted = false;
    }
    SECTION("save slots cannot be rebound by the shop result") {
        after[1].slot = 3;
        accepted = false;
    }
    SECTION("a changed live participant cannot lose newer rewards") {
        ++scene.liveSave(2)->gold;
        accepted = false;
    }
    const auto liveBefore = scene.party();
    CHECK(commitImportedShop(scene, before, after) == accepted);
    CHECK(*scene.liveSave(0) == (accepted ? after[0].save : liveBefore[0].save));
    CHECK(*scene.liveSave(2) == (accepted ? after[1].save : liveBefore[1].save));
    CHECK(scene.actor(0)->position() == position);
    CHECK(scene.actor(2)->position() == secondPosition);
    CHECK(scene.simulationCamera() == camera);
    CHECK(imported->follows == follows);
    CHECK(scene.participants()[0].resultsCheckpoint == checkpoint);
    CHECK_FALSE(scene.spawning());
    CHECK(scene.world() == &fixture.world);
    fixture.fullHud();
}

TEST_CASE("Imported camera rendering retains the paused and disabled interpolation sentinel",
          "[legends][gameplay][camera][assets]") {
    GameplayFixture fixture;
    auto imported = std::make_shared<PlainImportedLevel>();
    const std::array party{member(0)};
    fixture.open(imported, plainFloor(), {0, 0, 0}, 0, party);
    imported->sampledView = imported->view;
    imported->sampledView->position.x += 10.0f;
    const auto follows = imported->follows;
    struct RenderCase {
        bool paused;
        f32 alpha;
        bool sampled;
    };
    constexpr std::array cases{RenderCase{false, .25f, true}, RenderCase{false, 1.0f, true},
                               RenderCase{true, .25f, false}, RenderCase{false, -1.0f, false},
                               RenderCase{true, -1.0f, false}};
    for (const auto& render : cases) {
        CAPTURE(render.paused, render.alpha);
        fixture.device.draws.clear();
        imported->cameraSamples.clear();
        imported->drawnCameras.clear();
        fixture.scene.render(fixture.device, Mat4{1}, 640, 448, render.paused, render.alpha);
        REQUIRE_FALSE(imported->drawnCameras.empty());
        const auto expected = render.sampled ? *imported->sampledView : imported->view;
        for (const auto& camera : imported->drawnCameras) {
            CHECK(camera == expected);
        }
        if (render.sampled) {
            CHECK(std::ranges::find(imported->cameraSamples, render.alpha) !=
                  imported->cameraSamples.end());
        } else {
            // A native 30 Hz camera may still interpolate at alpha=1 when the
            // caller advances at 60 Hz. Only the negative sentinel requests
            // its current tick pose for a paused or uninterpolated render.
            CHECK(
                std::ranges::all_of(imported->cameraSamples, [](f32 alpha) { return alpha < 0; }));
        }
        CHECK(imported->follows == follows);
        CHECK(fixture.scene.viewCamera() == imported->view);
    }
}

TEST_CASE("Imported maps use ordinary four-player controls inventory and localized HUD",
          "[legends][gameplay][imported-level][assets]") {
    GameplayFixture fixture;
    auto imported = std::make_shared<PlainImportedLevel>();
    std::array party{member(0), member(1), member(2), member(3)};
    auto& inventory = party[0].save.progress().inventory;
    inventory.potions = {1, 4};
    inventory.addPowerup(powerup::kWeapon, damage::kThreeWayShot, 0, -1, false);
    fixture.open(imported, plainFloor(), {0, 0, 0}, 0, party);
    // The tick finishing START was still held at entry. Advance the first
    // ordinary gameplay tick before checking the native party callback.
    fixture.step();
    auto& scene = fixture.scene;
    REQUIRE(scene.actorCount() == 4);
    REQUIRE(scene.participants().size() == 4);
    CHECK(scene.simulationCamera() == imported->view);
    CHECK(scene.viewCamera() == imported->view);
    CHECK(imported->partySize == 4);
    CHECK(imported->captures > 0);
    CHECK(imported->follows > 0);
    for (s32 player = 0; player < 4; ++player) {
        REQUIRE(scene.actor(player));
        REQUIRE(scene.animator(player));
        CHECK(scene.runtime(player)->slot == static_cast<usize>(player));
        CHECK(scene.status(player).active);
        CHECK(scene.status(player).name == party[static_cast<usize>(player)].save.name);
    }
    fixture.fullHud();

    PlayScene::Inputs attack;
    attack[0].attack = true;
    for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
        fixture.step(attack);
    }
    REQUIRE(scene.missiles().count() == 1);
    CHECK(scene.missiles().missile(0).potion == 0);
    for (s32 tick = 0; tick < 300 && scene.missiles().count() != 0; ++tick) {
        fixture.step();
    }

    PlayScene::Inputs select;
    select[0].selector.up = true;
    fixture.step(select);
    for (s32 tick = 0; tick < 90 && !scene.selector(0).showing(); ++tick) {
        fixture.step();
    }
    REQUIRE(scene.selector(0).showing());
    fixture.step(select);
    CHECK(PowerupEffects::of(scene.actor(0)->save().progress().inventory).shots() == 3);
    for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
        fixture.step(attack);
    }
    REQUIRE(scene.missiles().count() == 3);
    for (s32 tick = 0; tick < 180; ++tick) {
        fixture.step();
    }

    PlayScene::Inputs potion;
    potion[0].usePotion = true;
    for (s32 tick = 0; tick < 90 && scene.actor(0)->save().progress().inventory.potions.size() == 2;
         ++tick) {
        fixture.step(potion);
    }
    CHECK(scene.actor(0)->save().progress().inventory.potions == std::vector<s32>{1});
    CHECK(scene.actor(1)->save().progress().inventory.potions.empty());
    CHECK(scene.effects().count() > 0);
    for (s32 tick = 0; tick < 60; ++tick) {
        fixture.step(potion);
    }
    CHECK(scene.actor(0)->save().progress().inventory.potions == std::vector<s32>{1});

    // Move a different seat. Native yaw affects actual movement, not just drawing.
    const auto before = scene.actor(3)->position();
    PlayScene::Inputs move;
    move[3].move = {{0, 1}, 1};
    for (s32 tick = 0; tick < 40; ++tick) {
        fixture.step(move);
    }
    const Vec3 delta = scene.actor(3)->position() - before;
    CHECK(delta.x > 1);
    CHECK(std::abs(delta.z) < .1f);
    CHECK(imported->partyUpdates > 0);
    CHECK_FALSE(scene.leaving());
    CHECK(scene.canPause(0));
    scene.close();
}

TEST_CASE("PlayScene routes native scenery selection and highlighting through its adapter",
          "[legends][gameplay][selection][assets]") {
    GameplayFixture fixture;
    auto imported = std::make_shared<PlainImportedLevel>();
    imported->picked = ImportedObjectHit{{.id = 99, .name = "ELEV1"}, .001f};
    const std::array party{member(0)};
    fixture.open(imported, plainFloor(), Vec3{0}, 0, party);
    CHECK(fixture.world.layout().objects().empty());
    CHECK_FALSE(fixture.scene.pick(Vec2{.5f})); // no presented view yet
    fixture.scene.render(fixture.device, Mat4{1}, 640, 448);
    const auto picked = fixture.scene.pick(Vec2{.5f});
    REQUIRE(picked);
    CHECK(picked->kind == SceneSelection::Kind::Object);
    CHECK(picked->id == 99);
    CHECK(picked->name == "ELEV1");
    fixture.scene.select(picked);
    fixture.scene.render(fixture.device, Mat4{1}, 640, 448);
    CHECK(imported->highlighted == 99);
    CHECK_FALSE(fixture.scene.pick(Vec2{-1}));
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

TEST_CASE("Native N64 Tower runs inside PlayScene with native camera scenery and GDL combat",
          "[legends][gameplay][tower][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter);
    REQUIRE(adapter->cameraConfig());
    const auto partyMember = member(0);
    ClassDataSet classes;
    REQUIRE(classes.load(fixture.root / "pdata"));
    LegendsTraversal probe;
    probe.open(map, partyMember.player, partyMember.save,
               classes.stats(partyMember.save.character));
    REQUIRE(probe.authoredStart());
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    const std::array party{partyMember};
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    auto& scene = fixture.scene;
    REQUIRE(scene.world() == &fixture.world);
    REQUIRE(scene.actorCount() == 1);
    REQUIRE(scene.animator(0));
    REQUIRE(imported->camera());
    CHECK(scene.simulationCamera() == *imported->camera());
    CHECK(scene.simulationCamera().pitch == Approx(.05f));
    CHECK(scene.actor(0)->position().x == Approx(-4.5f));
    CHECK(scene.actor(0)->position().z == Approx(-15.5f));
    CHECK_FALSE(scene.status(1).active);
    CHECK_FALSE(scene.status(2).active);
    CHECK_FALSE(scene.status(3).active);
    fixture.fullHud();
    const auto worldDraws = std::ranges::count_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.depthTest && draw.texture != &fixture.device.whiteTexture() &&
               !draw.vertices.empty();
    });
    CHECK(worldDraws > 10);

    PlayScene::Inputs attack;
    attack[0].attack = true;
    bool threw = false;
    for (s32 tick = 0; tick < 90 && !threw; ++tick) {
        fixture.step(attack);
        threw = scene.missiles().count() != 0;
    }
    REQUIRE(threw);
    for (s32 tick = 0; tick < 90; ++tick) {
        fixture.step();
    }

    // Sumner's body lives in the native script rather than worlds.wad. Exercise
    // the adapter through PartyMotion, not just its standalone cylinder helper.
    REQUIRE(scene.relocate({10, 0, -8}));
    for (s32 tick = 0; tick < 90; ++tick) {
        const f32 relative = -kPi / 2 - scene.simulationCamera().yaw;
        PlayScene::Inputs move;
        move[0].move = {{std::sin(relative), std::cos(relative)}, 1};
        fixture.step(move);
    }
    const auto position = scene.actor(0)->position();
    CHECK(position.x >= 6 + 1.5f + scene.actor(0)->radius() - .02f);
    CHECK(position.x < 10);
    CHECK(position.z == Approx(-8).margin(.05f));
    CHECK(scene.simulationCamera() == *imported->camera());
    CHECK_FALSE(scene.leaving());
    CHECK(scene.canPause(0));
    scene.close();
}

TEST_CASE("Tower completion portals survive real arrival and admit their actual destination",
          "[legends][gameplay][tower-special-gameplay][assets]") {
    const bool cathedral = GENERATE(false, true);
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory))
        SKIP("Native N64 Tower extraction is not installed");
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    auto visitor = member(0);
    auto& progress = visitor.save.progress().legendsProgress(LegendsPlatform::N64);
    progress.runestones = 0x1fff;
    progress.shards = 0xf;
    visitor.save.progress().health = 9000;
    const std::array party{visitor};
    ClassDataSet classes;
    REQUIRE(classes.load(fixture.root / "pdata"));
    LegendsTraversal probe;
    probe.open(map, visitor.player, visitor.save, classes.stats(visitor.save.character));
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    imported->initializeParty(party);
    // Open at the scripted contact, but through the actual START/materialization
    // path. No manual updateParty or forced departure is allowed here.
    const Vec3 centre = cathedral ? Vec3{0, 1, -30} : Vec3{1.5f, 0, 14};
    fixture.open(imported, probe.collision(), centre, 0, party, map.minBounds, map.maxBounds,
                 false);
    for (s32 tick = 0; tick < 600 && fixture.entering(); ++tick) {
        REQUIRE_FALSE(imported->destination());
        fixture.step();
    }
    REQUIRE_FALSE(fixture.entering());
    REQUIRE_FALSE(imported->destination());
    // A complete charge/dwell interval still remains after arrival control is
    // released; a held START cannot pre-charge an exit behind the arrival picture.
    for (s32 tick = 0; tick < 90; ++tick) {
        fixture.step();
        REQUIRE_FALSE(imported->destination());
    }
    if (!cathedral) {
        std::vector<std::vector<u8>> fireImages;
        const auto bank = directory.parent_path().parent_path() / "bosseffect/skodeath";
        for (const auto& texture : formats::decodeLegendsN64Textures(
                 readFile(bank / "objects.wad"), readFile(bank / "texdyn.rom"))) {
            if (!texture.image.pixels.empty())
                fireImages.push_back(texture.image.pixels);
        }
        fixture.device.draws.clear();
        fixture.scene.render(fixture.device, Mat4{1}, 640, 448);
        usize visible = 0;
        const auto project = [](const test::RecordedDraw& draw, usize index) {
            const Vec4 clip = draw.transform * Vec4{draw.vertices[index].position, 1};
            return clip.w > 0 ? Vec3{clip} / clip.w : Vec3{-100000};
        };
        const auto cross2 = [](Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; };
        const auto texelAlpha = [](const test::RecordedDraw& draw, Vec2 uv) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            if (!texture || texture->pixels.empty())
                return 1.0f;
            uv = uv * draw.state.uvScale + draw.state.uvOffset;
            uv -= glm::floor(uv);
            const auto x =
                std::min(static_cast<u32>(uv.x * texture->width()), texture->width() - 1);
            const auto y =
                std::min(static_cast<u32>(uv.y * texture->height()), texture->height() - 1);
            return static_cast<f32>(texture->pixels[(y * texture->width() + x) * 4 + 3]) / 255;
        };
        // Check the real scene's camera, transformed geometry, texel coverage,
        // culling and existing opaque depth, not merely a submitted bank pointer.
        for (const auto& fire : fixture.device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(fire.texture);
            if (!texture || std::ranges::find(fireImages, texture->pixels) == fireImages.end())
                continue;
            REQUIRE_FALSE(fire.state.cullBack);
            for (usize at = 0; at + 2 < fire.vertices.size(); at += 3) {
                const auto a = project(fire, at), b = project(fire, at + 1),
                           c = project(fire, at + 2);
                const Vec3 sample = (a + b + c) / 3.0f;
                if (sample.x < 0 || sample.x >= 640 || sample.y < 0 || sample.y >= 448 ||
                    sample.z <= 0 || sample.z >= 1)
                    continue;
                const Vec2 uv =
                    (fire.vertices[at].uv + fire.vertices[at + 1].uv + fire.vertices[at + 2].uv) /
                    3.0f;
                if (texelAlpha(fire, uv) <= fire.state.alphaTest ||
                    fire.vertices[at].color.a == 0 || fire.vertices[at + 1].color.a == 0 ||
                    fire.vertices[at + 2].color.a == 0)
                    continue;
                bool blocked = false;
                for (const auto& surface : fixture.device.draws) {
                    if (!surface.state.depthWrite || !surface.state.depthTest || &surface == &fire)
                        continue;
                    // Translucent contributors can blend over the effect; this
                    // probe tests opaque coverage only, without inventing a GPU.
                    if (surface.state.blend != BlendMode::Opaque)
                        continue;
                    for (usize tri = 0; tri + 2 < surface.vertices.size(); tri += 3) {
                        const auto p = project(surface, tri), q = project(surface, tri + 1),
                                   r = project(surface, tri + 2);
                        const f32 determinant = cross2(Vec2{q - p}, Vec2{r - p});
                        if (std::abs(determinant) < .00001f)
                            continue;
                        const f32 v = cross2(Vec2{sample - p}, Vec2{r - p}) / determinant;
                        const f32 w = cross2(Vec2{q - p}, Vec2{sample - p}) / determinant;
                        const f32 u = 1 - v - w;
                        if (u < 0 || v < 0 || w < 0 ||
                            p.z * u + q.z * v + r.z * w <= sample.z + .000001f)
                            continue;
                        const Vec2 surfaceUv = surface.vertices[tri].uv * u +
                                               surface.vertices[tri + 1].uv * v +
                                               surface.vertices[tri + 2].uv * w;
                        blocked |= texelAlpha(surface, surfaceUv) > surface.state.alphaTest;
                    }
                }
                visible += !blocked;
            }
        }
        CHECK(visible > 0);
    }
    PlayOutcome outcome = PlayOutcome::Running;
    s32 ticks = 0;
    for (; ticks < 600 && outcome == PlayOutcome::Running; ++ticks) {
        outcome = fixture.scene.update(1.0 / 60, {});
        if (const auto departure = imported->departure()) {
            CHECK(departure->yaw == 0);
            CHECK(departure->scale == 1);
            CHECK(departure->skin == nullptr);
            CHECK(departure->keepVisible);
            CHECK_FALSE(departure->transportSound);
            if (!departure->finished)
                CHECK_FALSE(departure->covered(fixture.scene.transition().covering()));
        }
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(imported->destination());
    REQUIRE(*imported->destination() == (cathedral ? "boss1" : "hell1"));
    REQUIRE(imported->departure());
    REQUIRE(imported->departure()->covered(fixture.scene.transition().covering()));
    if (cathedral) {
        // This is exactly the admission predicate used by consoleLegendsOpen.
        // A completed native white departure must not require a second black cover.
        CHECK_FALSE(fixture.scene.transition().covering());
        CHECK(imported->departure()->whiteFade == 1);
    }
    CHECK(glm::distance(
              Vec2{fixture.scene.actor(0)->position().x, fixture.scene.actor(0)->position().z},
              Vec2{centre.x, centre.z}) < .1f);
    // Verify the named destination can bind the real shared gameplay host;
    // the app stages it before releasing the outgoing scene/resources.
    const auto destination = directory.parent_path() / *imported->destination();
    auto nextMap = nativeMap(destination);
    GameplayFixture arrived;
    auto nextAdapter =
        LegendsSceneAdapter::load(arrived.device, LegendsPlatform::N64, destination, nextMap);
    REQUIRE(nextAdapter);
    LegendsTraversal nextStart;
    nextStart.open(nextMap, visitor.player, visitor.save, classes.stats(visitor.save.character));
    auto next = std::make_shared<LegendsLevel>(arrived.device, nextMap, std::move(nextAdapter),
                                               destination, fixture.root);
    next->initializeParty(party);
    arrived.open(next, nextStart.collision(), nextStart.start(), nextStart.actor().yaw(), party,
                 nextMap.minBounds, nextMap.maxBounds, false);
    CHECK(arrived.scene.isOpen());
    CHECK(arrived.scene.spawning());
    CHECK_FALSE(next->destination());
}

TEST_CASE("Native opponents wait for the opening camera rather than render sampling",
          "[legends][gameplay][camera][legends-generators][assets]") {
    const auto root = test::assetOrSkip("PLAYERS").parent_path();
    for (const auto* stage : {"mount1", "tower2"}) {
        INFO(stage);
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / stage;
        if (!std::filesystem::is_directory(directory)) {
            SKIP("Native N64 extraction is not installed");
        }
        test::FakeRenderDevice device;
        auto map = nativeMap(directory);
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE(adapter->cameraConfig());
        LegendsTraversal probe;
        probe.open(map, 0, CharacterSave{});
        REQUIRE(probe.authoredStart());
        LegendsLevel imported{device, map, std::move(adapter), directory, root};
        CHECK_FALSE(imported.opponentsReady());
        const std::array subjects{CameraSubject{.feet = probe.start()}};
        imported.resetCamera(subjects);
        const bool tower = std::string_view{stage} == "tower2";
        CHECK(imported.opponentsReady() == tower);
        const auto initial = imported.camera();
        // Paused camera sampling and unrelated presentation updates cannot age
        // the intro or admit generators/placed enemies early.
        for (s32 frame = 0; frame < 240; ++frame) {
            (void)imported.camera(static_cast<f32>(frame % 11) / 10.0f);
            imported.capturePresentation();
            imported.update(1.0f / 60.0f, true);
        }
        CHECK(imported.camera() == initial);
        CHECK(imported.opponentsReady() == tower);
        for (s32 tick = 0; tick < 180; ++tick) {
            imported.followCamera(subjects, 1.0f / 60.0f);
        }
        REQUIRE(imported.opponentsReady());
        // Re-forming an already admitted party must not replay the entry gate.
        imported.resetCamera(subjects);
        CHECK(imported.opponentsReady());
    }
}

TEST_CASE("Native PlayScene holds placed enemies and generator broods through its opening pan",
          "[legends][gameplay][camera][legends-generators][intro-enemies][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Mountain extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter);
    REQUIRE(adapter->cameraConfig());
    REQUIRE_FALSE(adapter->actors().empty());
    REQUIRE_FALSE(adapter->generators().empty());
    const std::array party{member(0)};
    LegendsTraversal probe;
    probe.open(map, 0, party[0].save);
    REQUIRE(probe.authoredStart());

    // Put the player beside a real authored placement so actor admission is
    // eligible throughout the pan. Keeping every enemy outside its activation
    // range would make the pre-handoff assertion vacuous.
    const auto actor = std::ranges::min_element(adapter->actors(), {}, [&](const auto& value) {
        return glm::distance(value.spawn.position, probe.start());
    });
    REQUIRE(actor != adapter->actors().end());
    const auto nativeIndex = actor->nativeIndex;
    std::optional<Vec3> start;
    const f32 distance = std::min(8.0f, actor->activationRadius * .5f);
    for (const Vec3 direction :
         std::array{Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{-1, 0, 0}, Vec3{0, 0, -1}}) {
        Vec3 position = actor->spawn.position + direction * distance;
        const auto floor = probe.collision().floorAt(position, 4, 10);
        if (!floor) {
            continue;
        }
        position.y = floor->y;
        const auto clear = probe.collision().resolveWalls(
            position, probe.actor().radius(), position.y + PlayerActor::kFootClearance,
            position.y + probe.actor().height() - PlayerActor::kFootClearance);
        const std::array visitors{EnemyView{.player = 0, .position = position}};
        if (glm::distance(clear, position) < .001f && legendsActorReady(*actor, visitors, false)) {
            start = position;
            break;
        }
    }
    REQUIRE(start);
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    fixture.open(imported, probe.collision(), *start, probe.actor().yaw(), party, map.minBounds,
                 map.maxBounds, false);
    auto& scene = fixture.scene;
    const auto& placed = scene.opponents().importedActors();
    REQUIRE(placed.count() > 0);
    usize placement = 0;
    while (placement < placed.count() && placed.descriptor(placement).nativeIndex != nativeIndex) {
        ++placement;
    }
    REQUIRE(placement < placed.count());
    REQUIRE(placed.pending(placement));
    REQUIRE(scene.generators().count() > 0);
    REQUIRE_FALSE(imported->opponentsReady());
    std::vector<s32> countdowns;
    for (s32 id = 0; id < static_cast<s32>(scene.generators().count()); ++id) {
        countdowns.push_back(scene.generators().countdownOf(id));
        REQUIRE(scene.generators().bredOf(id) == 0);
    }
    s32 ordinaryHeldFrames = 0;
    for (s32 tick = 0; tick < 600 && (!imported->opponentsReady() || fixture.entering()); ++tick) {
        CAPTURE(tick);
        ordinaryHeldFrames += scene.spawning() ? 0 : 1;
        const std::array visitors{EnemyView{.player = 0, .position = scene.actor(0)->position()}};
        REQUIRE(legendsActorReady(placed.descriptor(placement), visitors, false));
        fixture.step();
        CHECK(scene.enemies().count() == 0);
        CHECK(scene.critters().count() == 0);
        for (usize index = 0; index < placed.count(); ++index) {
            CHECK(placed.pending(index));
        }
        for (s32 id = 0; id < static_cast<s32>(countdowns.size()); ++id) {
            CHECK(scene.generators().countdownOf(id) == countdowns[static_cast<usize>(id)]);
            CHECK(scene.generators().bredOf(id) == 0);
            CHECK(scene.generators().livingOf(id) == 0);
        }
    }
    REQUIRE(imported->opponentsReady());
    REQUIRE_FALSE(fixture.entering());
    // This must cover ordinary gameplay updates after GDL's materialisation,
    // not merely the pre-existing spawning() early return.
    REQUIRE(ordinaryHeldFrames > 0);
    for (s32 tick = 0; tick < 60 && placed.pending(placement); ++tick) {
        fixture.step();
    }
    REQUIRE_FALSE(placed.pending(placement));
    CHECK(scene.enemies().count() + scene.critters().count() > 0);

    // Bring the ordinary native follow camera to an authored generator. No
    // direct Generators::update/spawn call or synthetic brood bypass is used.
    const auto roster = imported->importedGenerators();
    const auto generator = std::ranges::min_element(roster, {}, [&](const auto& value) {
        return glm::distance(value.position, probe.start());
    });
    REQUIRE(generator != roster.end());
    const auto states = scene.opponents().importedGeneratorStates();
    const auto selected =
        std::ranges::find(states, generator->nativeIndex, &ImportedGeneratorState::nativeIndex);
    REQUIRE(selected != states.end());
    REQUIRE(selected->pool >= 0);
    const s32 pool = selected->pool;
    std::optional<Vec3> nearby;
    for (const Vec3 direction :
         std::array{Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, 0, -1}, Vec3{-1, 0, 0}}) {
        Vec3 position = generator->position + direction * 10.0f;
        const auto floor = fixture.world.collision().floorAt(position, 4, 10);
        if (!floor) {
            continue;
        }
        position.y = floor->y;
        const auto clear = fixture.world.collision().resolveWalls(
            position, scene.actor(0)->radius(), position.y + PlayerActor::kFootClearance,
            position.y + scene.actor(0)->height() - PlayerActor::kFootClearance);
        if (glm::distance(clear, position) < .001f) {
            nearby = position;
            break;
        }
    }
    REQUIRE(nearby);
    REQUIRE(scene.relocate(*nearby));
    REQUIRE(imported->opponentsReady());
    for (s32 tick = 0; tick < 180 && scene.generators().bredOf(pool) == 0; ++tick) {
        fixture.step();
    }
    REQUIRE(scene.generators().bredOf(pool) > 0);
    CHECK(scene.generators().livingOf(pool) > 0);
    CHECK(scene.generators().countdownOf(pool) > countdowns[static_cast<usize>(pool)]);
    scene.close();
}

TEST_CASE("Native Tower exits wait for the party then spin and shrink with native silver artwork",
          "[legends][gameplay][portal][tower][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter =
        LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map, 2);
    REQUIRE(adapter);
    const auto found = std::ranges::find_if(
        adapter->portals(), [](const auto& portal) { return portal.map == "mount1"; });
    REQUIRE(found != adapter->portals().end());
    const auto& portal = *found;
    const std::array party{member(0), member(1)};
    LegendsTraversal probe;
    probe.open(map, 0, party[0].save);
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    auto& scene = fixture.scene;
    REQUIRE(scene.noclipMove(0, portal.position - scene.actor(0)->position(), std::nullopt));
    // A fixed +X offset can lie past this wedge's outer wall. Parking there tests
    // the fall-rescue path, which correctly returns the player beside its partner
    // and can therefore enter the portal. Use clear native ground toward the hub.
    const auto* waitingActor = scene.actor(1);
    const Vec2 inward = glm::normalize(Vec2{-portal.position.x, -portal.position.z});
    Vec3 waiting = portal.position + Vec3{inward.x, 0, inward.y} * 12.0f;
    const auto floor = fixture.world.collision().floorAt(waiting, 4, 10);
    REQUIRE(floor);
    waiting.y = floor->y;
    const auto clear = fixture.world.collision().resolveWalls(
        waiting, waitingActor->radius(), waiting.y + PlayerActor::kFootClearance,
        waiting.y + waitingActor->height() - PlayerActor::kFootClearance);
    REQUIRE(glm::distance(waiting, clear) < .001f);
    REQUIRE_FALSE(touchesLegendsPortal(
        portal, {1, waiting, true, waitingActor->radius(), waitingActor->height()}));
    REQUIRE(scene.noclipMove(1, waiting - waitingActor->position(), std::nullopt));
    for (s32 tick = 0; tick < 180; ++tick) {
        fixture.step();
        CHECK_FALSE(scene.leaving());
        CHECK_FALSE(imported->travelRequested());
        CAPTURE(tick, waiting.x, waiting.y, waiting.z, scene.actor(1)->position().x,
                scene.actor(1)->position().y, scene.actor(1)->position().z);
        REQUIRE_FALSE(
            touchesLegendsPortal(portal, {1, scene.actor(1)->position(), true,
                                          scene.actor(1)->radius(), scene.actor(1)->height()}));
    }
    REQUIRE(touchesLegendsPortal(portal, {0, scene.actor(0)->position(), true,
                                          scene.actor(0)->radius(), scene.actor(0)->height()}));
    CHECK_FALSE(touchesLegendsPortal(portal, {1, scene.actor(1)->position(), true,
                                              scene.actor(1)->radius(), scene.actor(1)->height()}));
    // Join the first player without overlapping bodies. Contact is still tested
    // by the native portal through the ordinary post-movement scene callback.
    REQUIRE(scene.noclipMove(1, portal.position + Vec3{2, 0, 0} - scene.actor(1)->position(),
                             std::nullopt));
    s32 waited = 0;
    for (; waited < 240 && !scene.leaving(); ++waited) {
        fixture.step();
    }
    REQUIRE(scene.leaving());
    REQUIRE(imported->destination());
    CHECK(*imported->destination() == "mount1");
    CHECK(waited >= 120);
    CHECK_FALSE(scene.departure().started()); // The ordinary GDL sink remains inactive.
    REQUIRE(imported->departure());
    CHECK_FALSE(imported->departure()->finished);
    CHECK(imported->departure()->scale == 1);
    REQUIRE(imported->departure()->skin);
    const auto* skin = imported->departure()->skin;
    const auto sourceTextures = formats::decodeLegendsN64Textures(
        readFile(directory.parent_path().parent_path() / "static/objects.wad"));
    const auto nativeSkin = std::ranges::find(sourceTextures, std::string{"CHROMESILVER"},
                                              &formats::LegendsN64Texture::name);
    REQUIRE(nativeSkin != sourceTextures.end());
    const auto* recordedSkin = dynamic_cast<const test::FakeTexture*>(skin);
    REQUIRE(recordedSkin);
    CHECK(recordedSkin->pixels == nativeSkin->image.pixels);
    CHECK_FALSE(scene.canPause(0));
    CHECK_FALSE(scene.canPause(1));
    const auto feet0 = scene.actor(0)->position();
    const auto feet1 = scene.actor(1)->position();
    const auto departingView = imported->camera();
    REQUIRE(departingView);
    fixture.device.draws.clear();
    scene.render(fixture.device, Mat4{1}, 640, 448, false, 1);
    const auto firstBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(firstBody != fixture.device.draws.end());
    // TreeModel bakes placement into submitted vertices; the draw matrix is
    // only the camera projection. Measure the body itself about its unchanged feet.
    const auto originalVertices = firstBody->vertices;
    REQUIRE(std::ranges::any_of(originalVertices, [&](const auto& vertex) {
        return glm::distance(vertex.position, feet0) > .1f;
    }));
    const auto texturesBeforeDeparture = fixture.device.texturesCreated;
    const auto* runtime = scene.runtime(0);
    REQUIRE(runtime);
    REQUIRE(runtime->figure);
    const auto beforeAnimation = runtime->figure->animator().player().checkpoint();
    const auto beforeAction = runtime->figure->animator().action();
    const auto beforeMissiles = scene.missiles().count();
    PlayScene::Inputs held;
    held[0].attack = true;
    held[0].move = {{0, 1}, 1};
    held[1].move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 12; ++tick) {
        fixture.step(held);
    }
    CHECK(scene.actor(0)->position() == feet0);
    CHECK(scene.actor(1)->position() == feet1);
    CHECK(scene.departure().displacement().y == 0);
    const auto departure = imported->departure();
    REQUIRE(departure);
    CHECK(departure->yaw == Approx(9.3f * .2f));
    CHECK(departure->scale == Approx(.91f));
    CHECK_FALSE(departure->finished);
    CHECK(imported->camera(0)->position == departingView->position);
    CHECK(imported->camera(1)->position == departingView->position);
    CHECK(imported->camera(0)->yaw == departingView->yaw);
    CHECK(imported->camera(1)->pitch == departingView->pitch);
    const auto& animator = runtime->figure->animator();
    const auto afterAnimation = animator.player().checkpoint();
    CHECK(afterAnimation.sequence == beforeAnimation.sequence);
    CHECK((afterAnimation.time != beforeAnimation.time ||
           afterAnimation.generation != beforeAnimation.generation));
    CHECK(animator.action() == beforeAction);
    CHECK_FALSE(animator.released());
    CHECK_FALSE(animator.strongReleased());
    CHECK_FALSE(animator.superReleased());
    CHECK_FALSE(animator.meleeStruck());
    CHECK(animator.itemReleased() == PlayerDeed::None);
    CHECK_FALSE(runtime->figure->familiarReleased());
    CHECK(scene.missiles().count() == beforeMissiles);
    fixture.device.draws.clear();
    scene.render(fixture.device, Mat4{1}, 640, 448, false, 1);
    const auto smallerBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(smallerBody != fixture.device.draws.end());
    const auto smallerVertices = smallerBody->vertices;
    const f32 renderedScale = imported->departure(1)->scale;
    // Compare against the same CURRENT animated pose at ordinary placement, not
    // against an earlier frame. The native exit does not freeze the body tree.
    fixture.device.draws.clear();
    const auto worn = PowerupEffects::of(runtime->actor.save().progress().inventory);
    const auto body = PlayerFigure::bodyPlacement(PartyFigures::presentationBody(*runtime, 1),
                                                  runtime->actor.save(), worn);
    const auto camera = CameraFrame::of(*imported->camera(1));
    runtime->figure->setSkinTexture(skin);
    runtime->figure->draw(fixture.device, Mat4{1}, body, fixture.world.lighting(), 1, false,
                          &camera, 1);
    runtime->figure->setSkinTexture(nullptr);
    const auto unscaledBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(unscaledBody != fixture.device.draws.end());
    REQUIRE(smallerVertices.size() == unscaledBody->vertices.size());
    for (usize vertex = 0; vertex < smallerVertices.size(); ++vertex) {
        const Vec3 original = unscaledBody->vertices[vertex].position - feet0;
        const Vec3 smaller = smallerVertices[vertex].position - feet0;
        CHECK(glm::length(smaller) == Approx(glm::length(original) * renderedScale).margin(.001f));
        CHECK(smaller.y == Approx(original.y * renderedScale).margin(.001f));
    }
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 tick = 0; tick < 240 && outcome == PlayOutcome::Running; ++tick) {
        outcome = scene.update(1.0 / 60, held);
    }
    CHECK(outcome == PlayOutcome::Travel);
    CHECK_FALSE(scene.departure().started());
    REQUIRE(imported->departure());
    CHECK(imported->departure()->finished);
    CHECK(imported->departure()->scale == Approx(.1f));
    CHECK(fixture.device.texturesCreated == texturesBeforeDeparture);
    CHECK(scene.transition().covering());
    CHECK(*imported->destination() == "mount1");
    // Common travel intent contains no forged GameCube realm or completion.
    CHECK(scene.destination().realmId == -1);
    scene.close();
}

TEST_CASE("Native scroll pickups open the shared scroll and hold ordinary gameplay",
          "[legends][gameplay][scrolls][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Mountain extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    const auto pickup = std::ranges::find_if(adapter->pickups(), [](const auto& value) {
        return value.offer.kind == static_cast<s32>(ItemKind::Scroll) && !value.scrollPages.empty();
    });
    REQUIRE(pickup != adapter->pickups().end());
    const auto position = pickup->position;
    const auto pickupIndex = static_cast<usize>(pickup - adapter->pickups().begin());
    const std::array party{member(0)};
    LegendsTraversal probe;
    probe.open(map, 0, party[0].save);
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    REQUIRE(
        fixture.scene.noclipMove(0, position - fixture.scene.actor(0)->position(), std::nullopt));
    fixture.step();
    REQUIRE(fixture.scene.scroll().active());
    REQUIRE(imported->adapter().pickups()[pickupIndex].collected);
    CHECK_FALSE(fixture.scene.scrollLook()); // No accidental GDL message identity.
    const auto heldPosition = fixture.scene.actor(0)->position();
    PlayScene::Inputs move;
    move[0].move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 20; ++tick) {
        fixture.step(move);
    }
    CHECK(fixture.scene.actor(0)->position() == heldPosition);
    CHECK_FALSE(imported->travelRequested());
    fixture.scene.close();
}

/** Keep the native fixture pipeline intact while excluding unrelated enemies,
 * opening-camera timing and pickups from these health/physical-strike tests. */
class NativeFixtureLevel final : public ImportedLevel {
public:
    explicit NativeFixtureLevel(std::shared_ptr<LegendsLevel> native) : level(std::move(native)) {}
    std::shared_ptr<LegendsLevel> level;
    std::vector<ImportedHazardHit> delivered;
    usize physicalHits = 0;
    usize worldHits = 0;
    s32 lastWorldAttempt = -1;
    s32 lastTargetAttempt = -1;
    struct WorldImpact {
        usize tick = 0;
        f32 power = 0;
        bool stillVisible = false;
    };
    std::vector<WorldImpact> worldImpacts;
    usize updates = 0;
    bool hazards = true;
    bool targetBodies = true;
    bool nativeCamera = false;
    void resetCamera(std::span<const CameraSubject> subjects) override {
        if (nativeCamera)
            level->resetCamera(subjects);
    }
    void followCamera(std::span<const CameraSubject> subjects, f32 seconds) override {
        if (nativeCamera)
            level->followCamera(subjects, seconds);
    }
    std::optional<WorldCamera> camera(f32 alpha = -1) const override {
        return nativeCamera ? level->camera(alpha) : std::nullopt;
    }
    std::optional<Vec3> cameraAttention() const override {
        return nativeCamera ? level->cameraAttention() : std::nullopt;
    }
    void capturePresentation() override { level->capturePresentation(); }
    void update(f32 seconds, bool stopped) override {
        ++updates;
        level->update(seconds, stopped);
    }
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
              TreeModel::Pass pass, f32 alpha) const override {
        level->draw(device, clip, camera, pass, alpha);
    }
    void bindCollision(WorldCollision& collision) const override {
        level->bindCollision(collision);
    }
    void syncCollision(WorldCollision& collision) const override {
        level->syncCollision(collision);
    }
    void watch(const ViewVolume& view) override { level->watch(view); }
    void updateParty(std::span<PlayerActor*> players, f32 seconds, bool activate) override {
        level->updateParty(players, seconds, activate);
    }
    std::vector<ImportedHazardHit> takeHazardHits() override {
        auto hits = level->takeHazardHits();
        if (!hazards) {
            return {};
        }
        delivered.insert(delivered.end(), hits.begin(), hits.end());
        return hits;
    }
    std::vector<ImportedTarget> targets() const override {
        return targetBodies ? level->targets() : std::vector<ImportedTarget>{};
    }
    bool strike(s32 id, f32 power, u32 flags, s32 player) override {
        lastTargetAttempt = id;
        const bool accepted = level->strike(id, power, flags, player);
        physicalHits += accepted ? 1 : 0;
        return accepted;
    }
    bool strikeWorldObject(s32 object, f32 power, u32 flags, s32 player) override {
        lastWorldAttempt = object;
        const bool accepted = level->strikeWorldObject(object, power, flags, player);
        worldHits += accepted ? 1 : 0;
        if (accepted) {
            worldImpacts.push_back(
                {updates, power, level->adapter().worldObjectVisible(static_cast<usize>(object))});
        }
        return accepted;
    }
};

TEST_CASE("Native fire and Ice steam damage through PlayScene without bypassing immunity",
          "[legends][gameplay][native-traps][native-hazard-health][assets]") {
    GameplayFixture fixture;
    const auto mapName = GENERATE("mount1", "ice1", "ice2", "ice3", "ice4");
    CAPTURE(mapName);
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / mapName;
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Mountain extraction is not installed");
    }
    auto map = nativeMap(directory);
    const auto definitions = formats::decodeLegendsN64Traps(
        readFile(directory / "ipr.rom"),
        readFile(directory.parent_path().parent_path() / "items/generic/ptrap.rom"));
    const auto trap =
        std::string_view{mapName} == "mount1"
            ? std::ranges::find(definitions.traps, 9U, &formats::LegendsN64Trap::nativeIndex)
            : definitions.traps.begin();
    REQUIRE(trap != definitions.traps.end());
    REQUIRE(trap->damage > 0);
    formats::LegendsN64Fixture placement;
    placement.position = trap->position;
    placement.groundProjected = trap->groundProjected;
    const Vec3 position = formats::legendsN64FixturePosition(placement, map.collision);
    auto player = member(0);
    player.save.progress().armorAdd = 999;
    bool immune = false;
    SECTION("max armor still receives damage") {}
    SECTION("silver invulnerability still refuses the contact") {
        player.save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
        immune = true;
    }
    SECTION("levitation still refuses low native trap contacts") {
        player.save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, -1);
        immune = true;
    }
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    LegendsTraversal probe;
    probe.open(map, 0, player.save);
    auto native = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter), directory,
                                                 fixture.root);
    auto imported = std::make_shared<NativeFixtureLevel>(native);
    imported->hazards = false;
    const std::array party{player};
    fixture.open(imported, probe.collision(), probe.start(), 0, party, map.minBounds,
                 map.maxBounds);
    REQUIRE(
        fixture.scene.noclipMove(0, position - fixture.scene.actor(0)->position(), std::nullopt));
    imported->hazards = true;
    const auto before = fixture.scene.liveSave(0)->health();
    for (s32 tick = 0; tick < 1200 && imported->delivered.empty(); ++tick) {
        fixture.step();
    }
    REQUIRE(imported->delivered.size() == 1);
    CHECK(imported->delivered[0].damage > 0);
    if (std::string_view{mapName} == "mount1")
        CHECK(imported->delivered[0].damage == 2);
    CHECK(imported->delivered[0].armorAbsorbed);
    CHECK((imported->delivered[0].impact.flags & damage::kLow) != 0);
    CHECK(fixture.scene.liveSave(0)->health() ==
          before - (immune ? 0 : imported->delivered[0].damage));
    fixture.scene.close();
}

TEST_CASE("Every Ice4 steam vent reaches a grounded player with the native camera",
          "[legends][native-traps][ice4-vents][assets]") {
    GameplayFixture fixture;
    const auto directory = test::dataDirectory().parent_path() / "assets/n64/extracted/levels/ice4";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("N64 Ice4 extraction is not installed");
    auto map = nativeMap(directory);
    const auto definitions = formats::decodeLegendsN64Traps(
        readFile(directory / "ipr.rom"),
        readFile(directory.parent_path().parent_path() / "items/generic/ptrap.rom"));
    REQUIRE(definitions.traps.size() == 23);
    auto player = member(0);
    player.save.progress().armorAdd = 999;
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    LegendsTraversal probe;
    probe.open(map, 0, player.save);
    auto native = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter), directory,
                                                 fixture.root);
    auto imported = std::make_shared<NativeFixtureLevel>(native);
    imported->nativeCamera = true;
    const std::array party{player};
    fixture.open(imported, probe.collision(), probe.start(), 0, party, map.minBounds,
                 map.maxBounds);
    for (const auto& trap : definitions.traps) {
        CAPTURE(trap.nativeIndex, trap.position.x, trap.position.y, trap.position.z);
        formats::LegendsN64Fixture placement;
        placement.kind = formats::LegendsN64FixtureKind::Trap;
        placement.position = trap.position;
        placement.groundProjected = trap.groundProjected;
        const Vec3 position = formats::legendsN64FixturePosition(placement, map.collision);
        REQUIRE(fixture.scene.noclipMove(0, position - fixture.scene.actor(0)->position(),
                                         std::nullopt));
        imported->delivered.clear();
        bool visibleSteam = false;
        for (s32 tick = 0; tick < 1200 && (imported->delivered.empty() || !visibleSteam); ++tick) {
            fixture.step();
            const auto camera = native->camera();
            REQUIRE(camera);
            fixture.device.draws.clear();
            native->draw(fixture.device, Mat4{1}, *camera, TreeModel::Pass::Blended, .5f);
            for (const auto& draw : fixture.device.draws) {
                // Steam is the narrow, upright quad over this vent. Ignore the
                // flat base: merely drawing it would hide the original symptom.
                Vec3 lo{1e6f}, hi{-1e6f};
                for (const auto& vertex : draw.vertices) {
                    const Vec3 point{draw.transform * Vec4{vertex.position, 1}};
                    lo = glm::min(lo, point);
                    hi = glm::max(hi, point);
                }
                const Vec3 centre = (lo + hi) * .5f;
                if (hi.y - lo.y < 4 || hi.y - lo.y > 6 ||
                    glm::distance(Vec2{centre.x, centre.z}, Vec2{position.x, position.z}) > .5f)
                    continue;
                const auto& texture = dynamic_cast<const test::FakeTexture&>(*draw.texture);
                for (usize pixel = 3; pixel < texture.pixels.size(); pixel += 4)
                    visibleSteam |= texture.pixels[pixel] > 0;
            }
        }
        const Vec3 settled = fixture.scene.actor(0)->position();
        CAPTURE(position.y, settled.x, settled.y, settled.z);
        CHECK_FALSE(imported->delivered.empty());
        CHECK(visibleSteam);
    }
    fixture.scene.close();
}

TEST_CASE("Native shootwalls receive ordinary physical melee and missile world impacts",
          "[legends][gameplay][native-scenery][native-scenery-combat][assets]") {
    GameplayFixture fixture;
    const auto [mapName, faceIndex, fullHealth] =
        GENERATE(std::tuple{"mount2", 100U, 30}, std::tuple{"boss1", 422U, 150});
    CAPTURE(mapName);
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / mapName;
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Dagger Peak extraction is not installed");
    }
    auto map = nativeMap(directory);
    REQUIRE_FALSE(map.destructibles.empty());
    const auto wall = map.destructibles.front();
    const auto face = std::ranges::find(map.collision, faceIndex,
                                        &formats::LegendsMapCollisionTriangle::nativeIndex);
    REQUIRE(face != map.collision.end());
    REQUIRE(face->owner == static_cast<s32>(wall.object));
    REQUIRE(face->wall);
    const Vec3 centre = (face->vertices[0] + face->vertices[1] + face->vertices[2]) / 3.0f;
    auto player = member(0);
    player.save.progress().fightAdd = 999;
    LegendsTraversal probe;
    probe.open(map, 0, player.save);
    bool melee = false;
    SECTION("native collision face catches missiles before the supplemental body") {}
    SECTION("nearby authored surface accepts normal melee without firing") {
        melee = true;
    }
    const Vec3 outward = glm::normalize(Vec3{face->normal.x, 0, face->normal.z});
    const Vec3 start = centre + outward * (melee ? 2.0f : 10.0f) - Vec3{0, 2.5f, 0};
    const auto floor = probe.collision().floorAt(start, 4, 10);
    REQUIRE(floor);
    const Vec3 feet{start.x, floor->y, start.z};
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    auto native = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter), directory,
                                                 fixture.root);
    auto imported = std::make_shared<NativeFixtureLevel>(native);
    imported->hazards = false;
    // Deliberately omit the small supplemental target: this is the larger native
    // triangle's world collision, which otherwise used to eat the projectile.
    imported->targetBodies = melee;
    const std::array party{player};
    const Vec3 forward = -outward;
    const f32 yaw = std::atan2(forward.x, forward.z);
    fixture.open(imported, probe.collision(), feet, yaw, party, map.minBounds, map.maxBounds);
    REQUIRE(native->adapter().worldObjectVisible(wall.object));
    const auto& visitor = *fixture.scene.actor(0);
    const Vec3 initialPosition = visitor.position();
    CAPTURE(feet.x, feet.y, feet.z, initialPosition.x, initialPosition.y, initialPosition.z,
            visitor.radius(), visitor.height(), visitor.reach(), visitor.yaw());
    if (melee) {
        const auto bodies = native->targets();
        const auto body = std::ranges::find_if(bodies, [&](const auto& target) {
            return std::ranges::any_of(target.surface, [&](const auto& triangle) {
                return triangle.object == static_cast<s32>(wall.object);
            });
        });
        REQUIRE(body != bodies.end());
        const MissileTarget surface{body->id, body->base, body->radius, body->height,
                                    body->surface};
        CAPTURE(body->base.y, body->height, body->radius,
                TargetAssist::distanceTo(visitor.position(), visitor.height(), surface));
        CHECK(TargetAssist::around(visitor.position(), visitor.height(), std::span{&surface, 1},
                                   visitor.reach() + 7, &fixture.world.collision()));
    }
    PlayScene::Inputs attack;
    attack[0].attack = true;
    attack[0].aimDirection = forward;
    bool fired = false;
    bool swung = false;
    for (s32 tick = 0; tick < 600 && native->adapter().worldObjectVisible(wall.object); ++tick) {
        fixture.step(attack);
        fired |= fixture.scene.missiles().count() != 0;
        swung |= fixture.scene.runtime(0)->figure->animator().meleeing();
    }
    const Vec3 finalPosition = visitor.position();
    CAPTURE(finalPosition.x, finalPosition.y, finalPosition.z,
            static_cast<s32>(fixture.scene.runtime(0)->figure->animator().action()),
            static_cast<s32>(fixture.scene.runtime(0)->life), imported->worldHits);
    CHECK(fired == !melee);
    CHECK(swung == melee);
    if (melee) {
        CHECK(imported->physicalHits > 0);
        CHECK(imported->worldHits == 0);
    } else {
        // A 999-strength ordinary missile is19.985 damage, not a one-hit
        // destroy command. Native SWAL truncates every impact to19.
        const auto scale = formats::legendsN64TrapScale(
            readFile(directory.parent_path().parent_path() / "_program/game.bin"), mapName);
        REQUIRE(scale);
        REQUIRE(static_cast<s32>(*scale * 30) == fullHealth);
        const f32 damage = PlayerMissiles::damageFor(kMaxStat);
        REQUIRE(damage == Approx(19.985f));
        const auto hits = static_cast<usize>((fullHealth + 18) / 19);
        CHECK(imported->worldHits == hits);
        REQUIRE(imported->worldImpacts.size() == hits);
        for (usize i = 0; i < hits; ++i) {
            CHECK(imported->worldImpacts[i].power == Approx(damage));
            CHECK(imported->worldImpacts[i].stillVisible == (i + 1 != hits));
            if (i > 0) {
                CHECK(imported->worldImpacts[i].tick > imported->worldImpacts[i - 1].tick);
            }
        }
        CHECK(imported->physicalHits == 0);
    }
    CHECK_FALSE(native->adapter().worldObjectVisible(wall.object));
    CHECK_FALSE(fixture.world.collision().solid(static_cast<s32>(wall.object)));
    fixture.scene.close();
}
TEST_CASE("Native pews receive live aimed weapon releases with their full target roster",
          "[legends][gameplay][native-scenery][native-scenery-projectiles][assets]") {
    const auto [character, level] =
        GENERATE(std::tuple{0, 1}, std::tuple{2, 99}, std::tuple{6, 50});
    const f32 side = GENERATE(-3.0f, 0.0f, 3.0f);
    const u32 faceIndex = GENERATE(422U, 432U);
    CAPTURE(character, level, side, faceIndex);
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Desecrated Temple extraction is not installed");
    }
    auto map = nativeMap(directory);
    REQUIRE_FALSE(map.destructibles.empty());
    const auto wall = map.destructibles.front();
    const auto face = std::ranges::find(map.collision, faceIndex,
                                        &formats::LegendsMapCollisionTriangle::nativeIndex);
    REQUIRE(face != map.collision.end());
    REQUIRE(face->owner == static_cast<s32>(wall.object));
    const Vec3 centre = (face->vertices[0] + face->vertices[1] + face->vertices[2]) / 3.0f;
    const Vec3 outward = glm::normalize(Vec3{face->normal.x, 0, face->normal.z});
    const Vec3 tangent{outward.z, 0, -outward.x};
    auto player = member(0);
    player.save.character = character;
    player.save.progress().experience = levelExperience(level);
    player.save.progress().fightAdd = 999;
    player.save.progress().magicAdd = 999;
    LegendsTraversal probe;
    probe.open(map, 0, player.save);
    const Vec3 start = centre + outward * 12.0f + tangent * side;
    const auto floor = probe.collision().floorAt(start, 4, 10);
    REQUIRE(floor);
    const Vec3 feet{start.x, floor->y, start.z};
    const Vec3 forward = glm::normalize(Vec3{centre.x - feet.x, 0, centre.z - feet.z});
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    auto native = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter), directory,
                                                 fixture.root);
    auto imported = std::make_shared<NativeFixtureLevel>(native);
    imported->hazards = false;
    // Unlike the fallback-only test, keep the complete shipping fixture targets.
    REQUIRE(imported->targetBodies);
    const std::array party{player};
    fixture.open(imported, probe.collision(), feet, std::atan2(forward.x, forward.z), party,
                 map.minBounds, map.maxBounds);
    const auto& actor = *fixture.scene.actor(0);
    const Vec3 actualFeet = actor.position();
    CAPTURE(feet.x, feet.y, feet.z, actualFeet.x, actualFeet.y, actualFeet.z, actor.height());
    ClassDataSet classes;
    REQUIRE(classes.load(fixture.root / "PDATA"));
    const auto* stats = classes.stats(character);
    REQUIRE(stats);
    PlayerMissiles::WeaponThrow probeWeapon;
    probeWeapon.character = character;
    probeWeapon.center = actor.followPoint();
    probeWeapon.position = actor.position();
    probeWeapon.direction = forward;
    probeWeapon.hand = stats->weaponOffset;
    const auto muzzle = PlayerMissiles::weaponLaunches(probeWeapon).front().position;
    const f32 radius = MissileSpec::of(character).radius;
    std::vector<WallContact> muzzleContacts;
    const auto clearMuzzle = fixture.world.collision().resolveWalls(
        muzzle, radius, muzzle.y - radius * .5f, muzzle.y + radius * .5f, &muzzleContacts,
        std::nullopt, WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
    const s32 muzzleOwner = muzzleContacts.empty() ? -1 : muzzleContacts.front().object;
    CAPTURE(muzzle.x, muzzle.y, muzzle.z, muzzleOwner, glm::distance(clearMuzzle, muzzle));
    PlayScene::Inputs input;
    input[0].attack = true;
    input[0].aimDirection = forward;
    bool fired = false;
    bool swung = false;
    Vec3 firstPosition{0};
    Vec3 firstVelocity{0};
    u32 firstFlags = 0;
    for (s32 tick = 0; tick < 900 && native->adapter().worldObjectVisible(wall.object); ++tick) {
        fixture.step(input);
        const auto& missiles = fixture.scene.missiles();
        if (!fired && missiles.count() != 0) {
            firstPosition = missiles.missile(0).position;
            firstVelocity = missiles.missile(0).velocity;
            firstFlags = missiles.missile(0).flags;
        }
        fired |= missiles.count() != 0;
        swung |= fixture.scene.runtime(0)->figure->animator().meleeing();
    }
    CAPTURE(firstPosition.x, firstPosition.y, firstPosition.z, firstVelocity.x, firstVelocity.y,
            firstVelocity.z, firstFlags, imported->physicalHits, imported->worldHits,
            imported->lastTargetAttempt, imported->lastWorldAttempt);
    CHECK_FALSE(swung);
    const bool occluded = faceIndex == 422 && side == 3 && character != 2;
    if (occluded) {
        // This approach crosses the unrelated, permanent end of the next pew.
        // The low Warrior release overlaps it; the Sorceress's descending shot
        // reaches it in flight. The taller Wizard clears it. Requiring damage
        // here would incorrectly shoot through PEW__NC249_231 to owner77.
        // Its collision is an anonymous static hull, not WOBJ85-owned faces:
        // CTRI87/85 carry flags20 and their CLST cells have no F000 owner marker.
        REQUIRE(map.objects.size() > 85);
        REQUIRE(map.objects[85].name == "PEW__NC249_231");
        REQUIRE(std::ranges::none_of(map.destructibles,
                                     [](const auto& target) { return target.object == 85; }));
        const u32 obstructionIndex = character == 0 ? 87U : 85U;
        const std::array<Vec3, 3> authoredVertices =
            character == 0 ? std::array{Vec3{26.9375f, 10, 20}, Vec3{20.1875f, 0, 26.5625f},
                                        Vec3{20.1875f, 10, 26.5625f}}
                           : std::array{Vec3{20.1875f, 10, 26.5625f}, Vec3{22.4375f, 0, 28},
                                        Vec3{22.4375f, 10, 28}};
        const auto authored = std::ranges::find(map.collision, obstructionIndex,
                                                &formats::LegendsMapCollisionTriangle::nativeIndex);
        REQUIRE(authored != map.collision.end());
        REQUIRE(authored->owner == -1);
        REQUIRE(authored->flags == 0x20);
        REQUIRE(authored->wall);
        REQUIRE(authored->vertices == authoredVertices);
        // Query only the independently specified native triangle. A different
        // anonymous wall blocking the full scene is not sufficient for this oracle.
        const Vec3 normal = glm::normalize(glm::cross(authoredVertices[1] - authoredVertices[0],
                                                      authoredVertices[2] - authoredVertices[0]));
        WorldCollision oracle;
        oracle.build(
            {CollisionTriangle{normal, authoredVertices, -1, WorldCollision::kWallQueryFlags}});
        if (character == 0) {
            CHECK_FALSE(fired);
            REQUIRE_FALSE(muzzleContacts.empty());
            REQUIRE(muzzleOwner == -1);
            CHECK(glm::distance(clearMuzzle, muzzle) > .001f);
            std::vector<WallContact> expectedContacts;
            const auto expectedClear = oracle.resolveWalls(
                muzzle, radius, muzzle.y - radius * .5f, muzzle.y + radius * .5f, &expectedContacts,
                std::nullopt, WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
            REQUIRE_FALSE(expectedContacts.empty());
            CHECK(glm::distance(expectedClear, clearMuzzle) < .001f);
            CHECK(glm::distance(expectedContacts.front().point, muzzleContacts.front().point) <
                  .001f);
        } else {
            REQUIRE(fired);
            const auto obstruction = fixture.world.collision().sweepSphere(
                firstPosition, firstPosition + firstVelocity * .1f, radius,
                WorldCollision::kMissileQueryFlags);
            REQUIRE(obstruction);
            const auto expected =
                oracle.sweepSphere(firstPosition, firstPosition + firstVelocity * .1f, radius,
                                   WorldCollision::kMissileQueryFlags);
            REQUIRE(expected);
            CHECK(obstruction->object == -1);
            CHECK(glm::distance(obstruction->point, expected->point) < .001f);
            CHECK(obstruction->time == Approx(expected->time).margin(.0001f));
        }
        CHECK(imported->lastWorldAttempt == -1);
        CHECK(imported->physicalHits == 0);
        CHECK(imported->worldHits == 0);
        CHECK(native->adapter().worldObjectVisible(wall.object));
        CHECK(fixture.world.collision().solid(static_cast<s32>(wall.object)));
    } else {
        CHECK(fired);
        CHECK(imported->physicalHits + imported->worldHits >= 8);
        CHECK_FALSE(native->adapter().worldObjectVisible(wall.object));
        CHECK_FALSE(fixture.world.collision().solid(static_cast<s32>(wall.object)));
    }
    fixture.scene.close();
}
TEST_CASE("A native pew obstructing the release muzzle receives its physical weapon damage",
          "[legends][gameplay][native-scenery][native-muzzle-impact][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Desecrated Temple extraction is not installed");
    }
    auto map = nativeMap(directory);
    REQUIRE_FALSE(map.destructibles.empty());
    const auto wall = map.destructibles.front();
    auto save = member(0).save;
    save.progress().fightAdd = 999;
    LegendsTraversal traversal;
    traversal.open(map, 0, save);
    auto collision = traversal.collision();
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    LegendsLevel level{fixture.device, map, std::move(adapter), directory, fixture.root};
    level.bindCollision(collision);
    ClassDataSet classes;
    REQUIRE(classes.load(fixture.root / "PDATA"));
    ItemArchive weapons;
    REQUIRE(weapons.load(fixture.root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    usize contacts = 0;
    arsenal.bind({fixture.device,
                  classes,
                  weapons,
                  collision,
                  effects,
                  audio,
                  nullptr,
                  {},
                  false,
                  false,
                  nullptr,
                  nullptr,
                  [&](s32 object, f32 power, u32 flags, s32 player) {
                      CHECK(object == static_cast<s32>(wall.object));
                      CHECK(power == Approx(19.985f));
                      CHECK(flags == 0);
                      CHECK(player == 0);
                      ++contacts;
                      return level.strikeWorldObject(object, power, flags, player);
                  }});
    PlayerActor actor;
    bool found = false;
    // Find a genuine grounded release blocked by this exact native face, not
    // a fabricated impact or a collision owner selected only by proximity.
    for (const auto& face : map.collision) {
        if (face.owner != static_cast<s32>(wall.object) || !face.wall) {
            continue;
        }
        const Vec3 centre = (face.vertices[0] + face.vertices[1] + face.vertices[2]) / 3.0f;
        const Vec3 outward = glm::normalize(Vec3{face.normal.x, 0, face.normal.z});
        for (const f32 distance : {1.5f, 2.0f, 2.5f, 3.0f, 4.0f}) {
            const Vec3 start = centre + outward * distance;
            const auto floor = collision.floorAt(start, 4, 10);
            if (!floor) {
                continue;
            }
            actor.spawn(0, save, classes.stats(0), {start.x, floor->y, start.z},
                        std::atan2(-outward.x, -outward.z));
            const auto weapon = arsenal.weaponThrow(actor, 0);
            REQUIRE(weapon);
            const auto launch = PlayerMissiles::weaponLaunches(*weapon).front();
            std::vector<WallContact> hits;
            const auto clear = collision.resolveWalls(
                launch.position, launch.spec->radius, launch.position.y - launch.spec->radius * .5f,
                launch.position.y + launch.spec->radius * .5f, &hits, std::nullopt,
                WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
            if (glm::distance(clear, launch.position) > .0001f && !hits.empty() &&
                hits.front().object == static_cast<s32>(wall.object)) {
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    REQUIRE(found);
    auto figure = PlayerFigure::load(fixture.device, fixture.root, save, false);
    REQUIRE(figure);
    for (usize hit = 0; hit < 8; ++hit) {
        arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
        CHECK(contacts == hit + 1);
        CHECK(arsenal.missiles().count() == 0);
        CHECK(level.adapter().worldObjectVisible(wall.object) == (hit != 7));
    }
    CHECK_FALSE(collision.solid(static_cast<s32>(wall.object)));
    arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
    CHECK(contacts == 8);
    CHECK(arsenal.missiles().count() == 1);
    arsenal.clear();
    effects.clear();
}
} // namespace
