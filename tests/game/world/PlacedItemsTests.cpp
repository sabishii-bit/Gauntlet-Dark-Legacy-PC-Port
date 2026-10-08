#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/app/Scenario.h"
#include "game/players/ItemPickup.h"
#include "game/world/Chests.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"
#include "game/world/PlacedItems.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("native pickup gas and fruit shadows composite over an already drawn chest",
          "[placed-items][pickup-occlusion][assets]") {
    const auto* name =
        GENERATE("ACIDICON", "FIREICON", "BREATHEF_ICON", "BANANNA", "APPLE", "CHERRY");
    CAPTURE(name);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("POWERUPS/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G2");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Chests chests;
    REQUIRE(chests.bind(device, world.layout(), world.items(), &world.collision()));
    chests.setPlayerCount(1);
    usize index = 0;
    while (index < chests.size() &&
           (!chests.chest(index).shown || chests.chest(index).subtype != Chests::kChest)) {
        ++index;
    }
    REQUIRE(index < chests.size());
    const auto& chest = chests.chest(index);
    const std::array visitors{ChestVisitor{chest.figure.position(), 1, 1}};
    chests.update(0, visitors);
    chests.update(3, {});
    REQUIRE(chest.state == Chests::kOpen);
    REQUIRE(world.placeItem(device, name, chest.figure.position() + Vec3{0, 3, 0}));
    const auto& item = world.placedItems().item(world.placedItems().size() - 1);
    REQUIRE(item.figure);
    world.placedItems().draw(device, Mat4{1}, {}); // apply native flipbook selection
    device.draws.clear();
    // The native pickup's own draw identifies its blended submeshes. This includes
    // fruit shadows sharing a mesh with their opaque food, not just separate nodes.
    item.model.draw(device, Mat4{1}, item.transform, {}, item.pose.matrices());
    std::vector<const Texture*> blendedTextures;
    for (const auto& draw : device.draws) {
        if (draw.state.alphaTest > 0) {
            blendedTextures.push_back(draw.texture);
        }
    }
    REQUIRE_FALSE(blendedTextures.empty());
    device.draws.clear();
    const WorldCamera camera{.position = chest.figure.position() + Vec3{0, 20, -30}};
    world.drawOpaque(device, Mat4{1}, camera);
    const usize chestStart = device.draws.size();
    chests.draw(device, Mat4{1}, {});
    const usize chestEnd = device.draws.size();
    REQUIRE(chestEnd > chestStart);
    world.drawDeferred(device, Mat4{1}, camera);
    usize found = 0;
    usize smoothed = 0;
    for (usize i = 0; i < device.draws.size(); ++i) {
        const auto& draw = device.draws[i];
        if (std::ranges::find(blendedTextures, draw.texture) == blendedTextures.end()) {
            continue;
        }
        ++found;
        CHECK(i >= chestEnd);
        CHECK(draw.state.depthTest);
        CHECK(draw.state.blend == BlendMode::Alpha);
        CHECK_FALSE(draw.state.usesAlphaToCoverage(4));
        smoothed += draw.state.usesSpriteSmoothing() ? 1 : 0;
    }
    CHECK(found >= blendedTextures.size());
    // Apples/cherries also have camera-facing CF_HILITE sprites; bananas do not.
    CHECK((smoothed > 0) == (std::string_view{name} != "BANANNA"));
    if (!std::string_view{name}.ends_with("ICON")) {
        for (usize node = 0; node < item.figure->nodes.size(); ++node) {
            if (item.figure->nodes[node].name == "CF_HILITE") {
                item.model.setMeshAlpha(node, 0);
            }
        }
        device.draws.clear();
        item.model.draw(device, Mat4{1}, item.transform, {}, item.pose.matrices(), nullptr, 1,
                        TreeModel::Pass::Blended);
        REQUIRE_FALSE(device.draws.empty()); // fruit's flat ground shadow is still drawn
        for (const auto& draw : device.draws) {
            CHECK_FALSE(draw.state.usesSpriteSmoothing());
        }
    }
}

TEST_CASE("the sprite filtering scenario starts beside the native G2 Fire Breath pickup",
          "[placed-items][smooth-sprites][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("POWERUPS/ANIM.PS2");
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/level-g2-sprite-filtering.json");
    REQUIRE(scenario.level == "G2");
    REQUIRE(scenario.tower.items.empty()); // use the retail placement, not an injected item
    REQUIRE(scenario.tower.position);
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(scenario.level);
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto start = *scenario.tower.position;
    world.setPlayerCount(1);
    REQUIRE(world.collision().floorAt(start, 4, 10, 1));
    const PlacedItems::Item* breath = nullptr;
    for (usize index = 0; index < world.placedItems().size(); ++index) {
        const auto& item = world.placedItems().item(index);
        if (item.name == "BREATHEF_ICON") {
            breath = &item;
            break;
        }
    }
    REQUIRE(breath != nullptr);
    REQUIRE(breath->takeable());
    CHECK(breath->position.x == Approx(-49.6328125f));
    CHECK(breath->position.z == Approx(-182.8125f));
    CHECK(glm::distance(start, breath->position) > 3);
    CHECK(glm::distance(start, breath->position) < 6);
    world.placedItems().draw(device, Mat4{1}, {});
    device.draws.clear();
    breath->model.draw(device, Mat4{1}, breath->transform, {}, breath->pose.matrices(), nullptr, 1,
                       TreeModel::Pass::Blended);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(std::ranges::any_of(device.draws,
                              [](const auto& draw) { return draw.state.usesSpriteSmoothing(); }));
}

TEST_CASE("the Cemetery Fire Parchment burns while lying in the level",
          "[placed-items][fire-parchment][native-assets][assets]") {
    const s32 rate = GENERATE(30, 60);
    const auto root =
        test::assetOrSkip("ITEMS/LEVELG/ANIM.PS2").parent_path().parent_path().parent_path();
    ItemArchive archive;
    ItemArchive powerups;
    WorldLayout layout;
    REQUIRE(archive.load(root / "ITEMS/LEVELG"));
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(layout.load(root / "LEVELS/LEVELG3"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive, &powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    usize parchment = items.size();
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "QUEST_PARCH") {
            parchment = i;
            break;
        }
    }
    REQUIRE(parchment < items.size());
    REQUIRE(items.item(parchment).visible);
    const auto slot = archive.textures.find("POOLFIRE");
    REQUIRE(slot);
    const auto* fire = &archive.textures.texture(device, *slot);
    const auto hasFire = [&](TreeModel::Pass pass) {
        device.draws.clear();
        items.draw(device, Mat4{1}, {}, nullptr, pass);
        return std::ranges::any_of(device.draws,
                                   [fire](const auto& draw) { return draw.texture == fire; });
    };
    for (s32 tick = 0; tick < rate * 10; ++tick) {
        items.update(1.0f / static_cast<f32>(rate));
        if (tick >= rate && tick % rate == 0) {
            CAPTURE(rate, tick);
            CHECK_FALSE(hasFire(TreeModel::Pass::DepthWriting));
            REQUIRE(hasFire(TreeModel::Pass::Effects));
        }
    }
    const std::array collectors{Collector{items.item(parchment).position, 0.1f, 0.1f}};
    const auto picked = items.collect(device, collectors);
    REQUIRE(std::ranges::any_of(
        picked, [parchment](const Pickup& pickup) { return pickup.item == parchment; }));
    CHECK_FALSE(hasFire(TreeModel::Pass::All));
}

TEST_CASE("the native Town Center banana shadow keeps its black alpha coverage",
          "[placed-items][alpha-item-shadow][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    const auto level = test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const auto banana = std::ranges::find_if(
        layout.itemInfos(), [](const ItemInfo& info) { return info.name == "BANANNA"; });
    REQUIRE(banana != layout.itemInfos().end());
    const auto info = static_cast<s32>(banana - layout.itemInfos().begin());
    REQUIRE(std::ranges::any_of(layout.itemInstances(), [info](const ItemInstance& instance) {
        return instance.info == info;
    }));

    ItemArchive archive;
    REQUIRE(archive.load(root / "POWERUPS"));
    const auto found = archive.trees.find("BANANNA");
    REQUIRE(found);
    const auto& tree = archive.trees.tree(*found);
    const auto object = archive.models.find("BANANNAGROUP2");
    REQUIRE(object);
    const auto& mesh = archive.models.mesh(*object);
    REQUIRE(mesh.parts.size() == 2);
    CHECK_FALSE(mesh.prelit);
    const auto& body = mesh.parts[0];
    const auto& shadow = mesh.parts[1];
    REQUIRE(shadow.indices.size() == 6);
    CHECK(body.texture != shadow.texture);
    CHECK(body.lightmap == 0);
    CHECK(shadow.lightmap == 0);
    CHECK_FALSE(archive.textures.entry(body.texture).translucent());
    CHECK(archive.textures.entry(shadow.texture).translucent());

    const auto& image = archive.textures.image(shadow.texture);
    REQUIRE(image.width == 32);
    REQUIRE(image.height == 32);
    bool black = true;
    u8 minimumAlpha = 255;
    u8 maximumAlpha = 0;
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const Color texel = image.pixel(x, y);
            black = black && texel.r == 0 && texel.g == 0 && texel.b == 0;
            minimumAlpha = std::min(minimumAlpha, texel.a);
            maximumAlpha = std::max(maximumAlpha, texel.a);
        }
    }
    CHECK(black);
    CHECK(minimumAlpha == 0);
    CHECK(maximumAlpha == 146); // native RGB5A3 palette's highest used alpha is 4/7

    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(tree, archive.models, archive.textures, device));
    model.draw(device, Mat4{1}, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    const auto& draw = device.draws[1];
    REQUIRE(draw.texture == &archive.textures.texture(device, shadow.texture));
    REQUIRE(draw.vertices.size() == 6);
    // pbSetDORegs selects the ordinary material for lightmap=0; setPrimAlpha then
    // uses source-alpha/inverse-source-alpha, not an additive or multiply pass.
    CHECK(draw.state.blend == BlendMode::Alpha);
    CHECK(draw.state.alphaTest == DrawState::kTranslucentAlphaTest);
    CHECK(draw.state.maskedTexture == nullptr);
    CHECK(draw.state.nextTexture == nullptr);
    for (const auto& vertex : draw.vertices) {
        CHECK(vertex.position.y == Approx(0.09375f));
        CHECK(vertex.color.a == 255);
    }
}

std::filesystem::path pickupPresentationFixture() {
    const auto root = test::scratchDirectory("pickup-presentation");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(root / "textures.json",
                  R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"COIN","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"IDLE","frames":4,"frameRate":30,
       "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]},
      {"name":"TREAS_JUNK","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
    writeTextFile(root / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "itemInfos":[{"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2,"armor":0,"collisionType":1}],
      "itemInstances":[]})");
    test::convertModelFixture(root);
    return root;
}

TEST_CASE("pickup emitters follow attachments textures and visibility independently of animation",
          "[placed-items][pickup-particles]") {
    const auto root = pickupPresentationFixture();
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"name":"WHITE","file":"white.png","width":2,"height":2},
      {"name":"FLAME0","file":"white.png","width":2,"height":2},
      {"name":"FLAME1","file":"white.png","width":2,"height":2}]})");
    test::convertModelFixture(root);
    writeTextFile(root / "animations.json", R"({
      "particles":[{"preset":2,"enables":16929,"texture":"FLAME0",
                    "particleLife":[0.1,0],"rate":[30,30,30,30]}],
      "textureAnimations":[{"name":"FLAME","texture":1,"source":1,"frames":2,"rate":1}],
      "trees":[{"name":"COIN","nodes":[
        {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
        {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}]},
        {"name":"TREAS_JUNK","nodes":[
          {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
    WorldLayout layout;
    ItemArchive archive;
    REQUIRE(layout.load(root));
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    items.bind(device, layout, nullptr, archives);
    items.setPlayerCount(1);
    REQUIRE(items.place(device, "COIN", {7, 0, 0}, nullptr));
    REQUIRE_FALSE(items.item(0).player.playing()); // a still tree must emit too
    items.update(1.0f / 30);
    const auto& field = items.item(0).particles.field();
    REQUIRE(field.size() == 1);
    REQUIRE(field.particleCount() > 0);
    CHECK(Vec3{field.emitter(0).node()[3]} == Vec3{7, 1, 0});
    const auto* first = field.textureOf(0);
    items.attach(0, glm::translate(Mat4{1}, Vec3{9, 4, 0}), false);
    items.update(1.0f / 30);
    CHECK(Vec3{field.emitter(0).node()[3]} == Vec3{9, 5, 0});
    CHECK(field.textureOf(0) != first);
    const auto count = field.particleCount();
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        device.draws.clear();
        items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::Effects, blend);
        REQUIRE(device.draws.size() == 1);
        CHECK_FALSE(device.draws[0].state.depthWrite);
        CHECK(field.particleCount() == count); // drawing never advances the emitter
    }
    REQUIRE(items.claim(items.item(0).position, 1, 1) == 0);
    for (s32 tick = 0; tick < 30; ++tick) {
        items.update(1.0f / 30);
    }
    CHECK(field.particleCount() == 0);
    device.draws.clear();
    items.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    REQUIRE(items.release(0, {14, 3, 0}, Vec3{0}, nullptr, 0));
    items.update(1.0f / 30);
    CHECK(field.particleCount() > 0);
    CHECK(Vec3{field.emitter(0).node()[3]} == Vec3{14, 4, 0});
    REQUIRE(items.blast(device, {14, 3, 0}, 4, 100).size() == 1);
    CHECK(items.item(0).name == "TREAS_JUNK");
    CHECK(items.item(0).particles.field().size() == 0); // no stale fire on a replacement
}

TEST_CASE("pickup presentation samples flight and fractional poses without changing collection",
          "[placed-items][presentation]") {
    const auto root = pickupPresentationFixture();
    WorldLayout layout;
    ItemArchive archive;
    REQUIRE(layout.load(root));
    REQUIRE(archive.load(root));
    WorldCollision collision;
    const std::array<Vec3, 3> floor{Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    collision.build({{{0, 1, 0}, floor, 0, 4}});
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    items.bind(device, layout, &collision, archives);
    items.setPlayerCount(1);
    REQUIRE(items.throwItem(device, "COIN", {7, 5, 0}, {60, 0, 0}, &collision, 2));
    const auto corner = [&](f32 blend) {
        device.draws.clear();
        items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, blend);
        REQUIRE(device.draws.size() == 1);
        return device.draws[0].vertices[0].position;
    };
    CHECK(corner(0) == Vec3{7, 5, 0}); // new drops have no prior occupant to interpolate
    items.update(1.0f / 60);
    const Mat4 nativePose = items.item(0).pose.matrices()[0];
    const Mat4 nativePlacement = items.item(0).transform;
    const Vec3 velocity = items.item(0).velocity;
    const f32 noGrab = items.item(0).noGrabSeconds;
    for (const f32 blend : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f}) {
        CAPTURE(blend);
        const Vec3 vertex = corner(blend);
        CHECK(vertex.x == Approx(7 + blend));
        CHECK(vertex.y == Approx(5 + 0.5f * blend));
        CHECK(items.item(0).pose.matrices()[0] == nativePose);
        CHECK(items.item(0).transform == nativePlacement);
        CHECK(items.item(0).velocity == velocity);
        CHECK(items.item(0).noGrabSeconds == noGrab);
        CHECK_FALSE(items.item(0).takeable());
    }
    const Vec3 native = corner(-1);
    CHECK(native.x == Approx(8));
    CHECK(native.y == Approx(5 + items.item(0).player.frame()));
    items.capturePresentation(); // a held scene does not update this owner
    const Vec3 held = corner(0);
    CHECK(corner(0.25f) == held);
    CHECK(corner(0.75f) == held);
    CHECK(corner(1) == held);
    items.discard(0);
    device.draws.clear();
    items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0);
    CHECK(device.draws.empty());
}

TEST_CASE("pickup presentation cuts changed containers figures releases teleports and loops",
          "[placed-items][presentation]") {
    const auto root = pickupPresentationFixture();
    WorldLayout layout;
    ItemArchive archive;
    REQUIRE(layout.load(root));
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    items.bind(device, layout, nullptr, archives);
    items.setPlayerCount(1);
    REQUIRE(items.place(device, "COIN", {7, 0, 0}, nullptr));
    const auto corner = [&](f32 blend) {
        device.draws.clear();
        items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, blend);
        REQUIRE(device.draws.size() == 1);
        return device.draws[0].vertices[0].position;
    };
    SECTION("attaching to a new container snaps but following the same socket interpolates") {
        items.capturePresentation();
        items.attach(0, glm::translate(Mat4{1}, Vec3{10, 0, 0}), true);
        CHECK(corner(0).x == Approx(10));
        items.capturePresentation();
        items.attach(0, glm::translate(Mat4{1}, Vec3{12, 0, 0}), true);
        CHECK(corner(0.5f).x == Approx(11));
        CHECK(items.item(0).position.x == Approx(12));
    }
    SECTION("a large external relocation never streaks across the level") {
        items.capturePresentation();
        items.attach(0, glm::translate(Mat4{1}, Vec3{100, 0, 0}), false);
        CHECK(corner(0).x == Approx(100));
    }
    SECTION("an explicit short platform cut snaps riders without changing their clocks") {
        items.update(1.0f / 60);
        items.capturePresentation();
        items.attach(0, glm::translate(Mat4{1}, Vec3{9, 0, 0}), false);
        const f32 nativeFrame = items.item(0).player.frame();
        const Mat4 nativePose = items.item(0).pose.matrices()[0];
        items.snapPresentation();
        for (const f32 blend : {0.0f, 0.25f, 0.75f, 1.0f}) {
            CHECK(corner(blend) == Vec3{9, nativeFrame, 0});
            CHECK(items.item(0).player.frame() == nativeFrame);
            CHECK(items.item(0).pose.matrices()[0] == nativePose);
        }
    }
    SECTION("a carried pickup reappears at its release rather than its old placement") {
        REQUIRE(items.claim({7, 0, 0}, 2, 2) == 0);
        items.capturePresentation();
        REQUIRE(items.release(0, {10, 0, 0}, {}, nullptr, 0));
        CHECK(corner(0).x == Approx(10));
    }
    SECTION("a replacement figure has no link to the previous animation") {
        items.update(2.0f / 30);
        items.capturePresentation();
        const auto changes = items.blast(device, {7, 0, 0}, 5, 100);
        REQUIRE(changes.size() == 1);
        CHECK(items.item(0).name == "TREAS_JUNK");
        CHECK(corner(0.5f).y == Approx(0));
    }
    SECTION("a wrapping animation does not sweep back through its final pose") {
        items.update(3.0f / 30);
        items.update(1.0f / 30);
        CHECK(items.item(0).player.frame() == 0);
        CHECK(corner(0.5f).y == Approx(0));
    }
}

TEST_CASE("pickups bind to authored floors and ride the current platform pose",
          "[game][world][placed-items][pickup-platform]") {
    const auto dir = test::scratchDirectory("pickup-platform");
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/FOOD.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(
        dir / "objects.json",
        R"({"objects":[{"index":0,"name":"FOOD","file":"models/FOOD.obj","meshTriangles":1}]})");
    writeFile(dir / "textures/FOOD.png", test::kTinyPng);
    writeTextFile(
        dir / "textures.json",
        R"({"defs":[],"bitmaps":[{"index":0,"name":"FOOD","file":"textures/FOOD.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(
        dir / "animations.json",
        R"({"trees":[{"name":"FOOD","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]}]})");
    // A nested platform's rest position includes its parent's translation.
    auto worldText = std::string{R"({"objects":[
        {"name":"ROOT","position":[100,10,50],"child":1},
        {"name":"LIFT","position":[0,0,0],"flags":4100}],
        "itemInfos":[{"type":1,"subtype":3,"name":"FOOD","radius":0.5,"height":2}],
        "itemInstances":[{"info":0,"position":[101,10,50],"minPlayers":1},
                         {"info":0,"position":[1,0,0],"minPlayers":1}]})"};
    // FloorPos also binds authored markers above or slightly below the floor.
    // A narrow player-step probe leaves these detached as the platform moves.
    const auto authoredY = GENERATE(10, 14, 7);
    CAPTURE(authoredY);
    worldText.replace(worldText.find("101,10,50"), 9, "101," + std::to_string(authoredY) + ",50");
    writeTextFile(dir / "world.json", worldText);
    WorldLayout layout;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    WorldCollision collision;
    const std::array<Vec3, 3> face{Vec3{-4, 0, -4}, Vec3{4, 0, -4}, Vec3{0, 0, 4}};
    collision.build({{{0, 1, 0}, face, 1, 4100}, {{0, 1, 0}, face, 2, 4}});
    collision.setMovingObjects(std::array<s32, 1>{1});
    const Mat4 initial = glm::translate(Mat4{1}, Vec3{120, -10, 50});
    collision.setObjectTransform(1, initial);
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    REQUIRE(items.bind(device, layout, &collision, archives));
    REQUIRE(items.size() == 2);
    REQUIRE(items.item(0).floor);
    CHECK(items.item(0).floor->object == 1);
    const Mat4 local = items.item(0).floor->local;
    CHECK(glm::distance(items.item(0).position, Vec3{121, -9.9f, 50}) < 0.0001f);
    CHECK_FALSE(items.item(1).floor);
    CHECK(items.item(1).position.y == Approx(0.1f));
    CHECK(collision.objectTransform(1) == initial); // binding never mutates live collision
    items.setPlayerCount(1);

    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{110, 5, 40}) * glm::rotate(Mat4{1}, 1.0f, Vec3{0, 1, 0});
    SECTION("rendered platform riders interpolate while pickup and floor queries stay current") {
        const Vec3 previous = items.item(0).position;
        items.capturePresentation();
        collision.setObjectTransform(1, moved);
        items.syncFloors();
        items.update(1.0f / 60);
        const Mat4 expected = moved * local;
        for (const f32 blend : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            device.draws.clear();
            items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, blend);
            REQUIRE(device.draws.size() == 2);
            const Vec3 drawn = device.draws[0].vertices[0].position;
            CHECK(glm::distance(drawn, glm::mix(previous, Vec3{expected[3]}, blend)) < 0.0001f);
            CHECK(items.item(0).transform == expected);
            CHECK(collision.objectTransform(1) == moved);
        }
    }
    SECTION("resting and hidden items follow translation and rotation without accumulating drift") {
        items.setPlayerCount(0);
        collision.setObjectTransform(1, moved);
        items.update(0);
        const Mat4 expected = moved * local;
        CHECK(glm::distance(items.item(0).position, Vec3{expected[3]}) < 0.0001f);
        for (s32 i = 0; i < 100; ++i) {
            items.syncFloors();
        }
        CHECK(items.item(0).transform == expected);
        CHECK(items.item(1).position == Vec3{1, 0.1f, 0});
        items.setPlayerCount(1);
        const std::array collectors{Collector{items.item(0).position, 1, 1}};
        const auto got = items.collect(device, collectors);
        REQUIRE(got.size() == 1);
        CHECK(got[0].item == 0);
        CHECK(got[0].position == Vec3{expected[3]});
        collision.setObjectTransform(1, initial);
        items.syncFloors();
        CHECK(items.item(0).transform == expected); // a taken item no longer rides
    }
    SECTION("container attachment takes ownership away from the floor") {
        const Mat4 chest = glm::translate(Mat4{1}, Vec3{5, 6, 7});
        items.attach(0, chest, true);
        collision.setObjectTransform(1, moved);
        items.syncFloors();
        CHECK_FALSE(items.item(0).floor);
        CHECK(items.item(0).transform == chest);
    }
    SECTION("claimed items detach and released items reattach only after landing") {
        const Vec3 position = items.item(0).position;
        REQUIRE(items.claim(position, 2, 2) == 0);
        CHECK_FALSE(items.item(0).floor);
        collision.setObjectTransform(1, moved);
        items.syncFloors();
        CHECK(items.item(0).position == position);
        REQUIRE(items.release(0, Vec3{110, 8, 40}, Vec3{0}, &collision, 0));
        CHECK(items.item(0).thrown);
        CHECK_FALSE(items.item(0).floor);
        for (s32 i = 0; i < 150; ++i) {
            items.update(1.0f / 30.0f);
        }
        REQUIRE_FALSE(items.item(0).thrown);
        REQUIRE(items.item(0).floor);
        CHECK(items.item(0).position.y == Approx(6));
        const Mat4 landed = items.item(0).floor->local;
        collision.setObjectTransform(1, initial);
        items.syncFloors();
        CHECK(glm::distance(items.item(0).position, Vec3{initial * landed[3]}) < 0.0001f);
    }
    SECTION("runtime drops bind to the current floor rather than its authored pose") {
        REQUIRE(items.place(device, "FOOD", Vec3{120, -10, 50}, &collision));
        REQUIRE(items.item(2).floor);
        CHECK(items.item(2).position.y == Approx(-9.9f));
        collision.setObjectTransform(1, moved);
        items.syncFloors();
        CHECK(glm::distance(items.item(2).position, Vec3{moved * Vec4{0, 0.1f, 0, 1}}) < 0.0001f);
    }
    SECTION("disabling floor collision during an animation preserves its child attachment") {
        collision.setSolid(1, false);
        items.syncFloors();
        REQUIRE(items.item(0).floor);
        collision.setObjectTransform(1, moved);
        items.syncFloors();
        CHECK(glm::distance(items.item(0).position, Vec3{(moved * local)[3]}) < 0.0001f);
    }
}

TEST_CASE("food poisoning preserves missing artwork and uses record kind rather than value",
          "[game][world][poison-food][blast-items]") {
    const bool missingFigure = GENERATE(false, true);
    const auto dir = test::scratchDirectory("placed-items-poison");
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/FOOD.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(
        dir / "objects.json",
        R"({"objects":[{"index":0,"name":"FOOD","file":"models/FOOD.obj","meshTriangles":1}]})");
    writeFile(dir / "textures/FOOD.png", test::kTinyPng);
    writeTextFile(
        dir / "textures.json",
        R"({"defs":[],"bitmaps":[{"index":0,"name":"FOOD","file":"textures/FOOD.png","width":2,"height":2,"flags":0}]})");
    const std::string trees =
        missingFigure
            ? R"({"trees":[{"name":"FOOD","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]}]})"
            : R"({"trees":[{"name":"FOOD","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]},{"name":"BADMEAT","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]}]})";
    writeTextFile(dir / "animations.json", trees);
    // A meat-kind record with a small healing value: classification is not value >= 100.
    writeTextFile(dir / "world.json",
                  R"({"objects":[{"name":"GROUND","position":[0,0,0]}],"itemInfos":[
        {"type":1,"subtype":3,"name":"FOOD","collisionType":1,"radius":0.5,"height":2,
         "armor":-2,"hitPoints":2,"value":10,"collisionOffset":[2,0,0]}],
        "itemInstances":[{"info":0,"position":[0,0,0],"minPlayers":1}]})");
    WorldLayout layout;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    REQUIRE(items.size() == 1);
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == 0); // hidden party instance
    items.setPlayerCount(1);
    CHECK(items.poisonFood(device, Vec3{0}, 1, 10) == 0); // collision centre is offset
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 2) == 0);
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == (missingFigure ? 0 : 1));
    CHECK(items.item(0).name == (missingFigure ? "FOOD" : "BADMEAT"));
    CHECK(items.item(0).value == (missingFigure ? 10 : -100));
    CHECK(items.item(0).takeable());
    items.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == 0);
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 4.99f).empty());
    CHECK(items.blast(device, Vec3{0}, 1, 5).empty());
    const auto changes = items.blast(device, Vec3{2, 0, 0}, 1, 5);
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().destroyed);
    CHECK_FALSE(items.item(0).takeable());
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 5).empty());
}

TEST_CASE("explosions destroy exposed food and powerups but preserve quest pickups",
          "[game][world][blast-items][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELG1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(0);
    const Vec3 origin{10000, 0, 10000};
    const usize first = items.size();
    for (const auto* name :
         {"APPLE", "FIREICON", "TREAS_GOLD", "KEY", "GEMBLUE", "SCROLL", "GARGEAGL", "HAM"}) {
        REQUIRE(items.place(device, name, origin, nullptr));
    }
    const usize held = items.size() - 1;
    items.attach(held, items.item(held).transform, true);
    REQUIRE(items.place(device, "APPLE", origin + Vec3{0, 20, 0}, nullptr));
    REQUIRE(items.place(device, "APPLE", origin + Vec3{20, 0, 0}, nullptr));
    // WorldExplosion's fallback carries fire/knockdown, not DMG_EXPLODE.
    CHECK(items.blast(device, origin, 6, 50, false).empty());
    CHECK_FALSE(items.item(first).taken);
    CHECK(items.item(first + 2).name == "TREAS_GOLD");
    REQUIRE(items.blast(device, origin, 6, 4.99f).empty());
    const auto changes = items.blast(device, origin, 6, 5);
    REQUIRE(changes.size() == 3);
    CHECK(changes[0].destroyed);
    CHECK(changes[1].destroyed);
    CHECK_FALSE(changes[2].destroyed);
    CHECK(items.item(first).taken);
    CHECK(items.item(first + 1).taken);
    const auto& junk = items.item(first + 2);
    CHECK(junk.name == "TREAS_JUNK");
    CHECK(junk.value == 10);
    REQUIRE(junk.figure == &powerups.trees.tree(*powerups.trees.find("TREAS_JUNK")));
    CHECK(junk.takeable());
    for (usize i = first + 3; i < items.size(); ++i) {
        CHECK_FALSE(items.item(i).taken);
    }
    CHECK(items.blast(device, origin, 6, 50).empty());
    items.update(0.1f);
    items.draw(device, Mat4{1}, {});
    CHECK_FALSE(device.draws.empty());
    const std::array collectors{Collector{origin, 1, 1}};
    const auto pickups =
        items.collect(device, collectors, [](const Pickup& pickup) -> std::optional<s32> {
            return pickup.subtype == static_cast<s32>(ItemKind::Gold) ? std::optional<s32>{0}
                                                                      : std::nullopt;
        });
    REQUIRE(pickups.size() == 1);
    CHECK(pickups.front().amount == 10);
    CHECK(items.item(first).taken); // destroyed food is never collectible again
}

TEST_CASE("gas poisons food models and pickup values without moving or consuming them",
          "[game][world][poison-food][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELG1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(0); // isolate newly placed food from authored level pickups
    const Vec3 origin{10000, 0, 10000};
    const usize apple = items.size();
    REQUIRE(items.place(device, "APPLE", origin, nullptr));
    const usize meat = items.size();
    REQUIRE(items.place(device, "CHICKEN", origin + Vec3{0, 0, 2}, nullptr));
    const usize protectedFood = items.size();
    REQUIRE(items.place(device, "HAM", origin, nullptr));
    items.attach(protectedFood, items.item(protectedFood).transform, true);
    const usize distant = items.size();
    REQUIRE(items.place(device, "APPLE", origin + Vec3{20, 0, 0}, nullptr));
    const usize above = items.size();
    REQUIRE(items.place(device, "APPLE", origin + Vec3{0, 20, 0}, nullptr));
    const usize potion = items.size();
    REQUIRE(items.place(device, "POT_BLU", origin, nullptr));
    const Mat4 transform = items.item(meat).transform;
    const s32 record = items.item(meat).info;
    REQUIRE(items.poisonFood(device, origin, 6.5f, 2) == 0);
    REQUIRE(items.poisonFood(device, origin, 0, 10) == 0);
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 2);
    CHECK(items.item(apple).name == "GAPPLE");
    CHECK(items.item(apple).value == -50);
    CHECK(items.item(meat).name == "BADMEAT");
    CHECK(items.item(meat).value == -100);
    CHECK(items.item(meat).info == record);
    CHECK(items.item(meat).transform == transform);
    CHECK(items.item(protectedFood).name == "HAM");
    CHECK(items.item(distant).name == "APPLE");
    CHECK(items.item(above).name == "APPLE");
    CHECK(items.item(potion).name == "POT_BLU");
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 0);
    for (const usize index : {apple, meat}) {
        const auto& item = items.item(index);
        REQUIRE(item.figure == &powerups.trees.tree(*powerups.trees.find(item.name)));
        REQUIRE(item.model.bound());
        REQUIRE(item.takeable());
    }
    items.update(0.1f);
    items.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    CharacterSave save;
    save.progress().health = 500;
    const std::array collectors{Collector{origin, 4, 1}};
    const auto collected =
        items.collect(device, collectors, [&](const Pickup& pickup) -> std::optional<s32> {
            if (pickup.subtype != static_cast<s32>(ItemKind::Food)) {
                return std::nullopt;
            }
            const auto result =
                takeItem(save, {pickup.subtype, pickup.amount, pickup.flags, pickup.strength});
            CHECK(result.hurt);
            CHECK_FALSE(result.ate);
            CHECK(result.message == 28);
            return result.left;
        });
    REQUIRE(collected.size() == 2);
    CHECK(save.health() == 350);
    CHECK(items.item(apple).taken);
    CHECK(items.item(meat).taken);
    // Chest contents become vulnerable only after release; taken food never returns.
    items.attach(protectedFood, items.item(protectedFood).transform, false);
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 1);
    CHECK(items.item(protectedFood).name == "BADMEAT");
    CHECK(items.item(apple).taken);
}

TEST_CASE("the tower's crystals stand on the floor for a party large enough",
          "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    WorldCollision collision;
    REQUIRE(collision.load(root / "LEVELS/LEVELL1", layout));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, &collision, archives));
    REQUIRE(items.size() >= 15);
    REQUIRE(items.visibleCount() == 0); // nobody in the party yet

    usize gems = 0;
    for (usize i = 0; i < items.size(); ++i) {
        const PlacedItems::Item& item = items.item(i);
        if (item.name != "GEMORANGE") {
            continue;
        }
        ++gems;
        REQUIRE(item.subtype == ItemInfo::kCrystal);
        REQUIRE(item.minPlayers == 1);
        REQUIRE(item.model.bound());
        // Lifted onto the floor near where the level put it.
        const ItemInstance& source = layout.itemInstances()[static_cast<usize>(item.instance)];
        REQUIRE(item.position.x == source.position.x);
        REQUIRE(std::abs(item.position.y - source.position.y) <=
                PlacedItems::kFloorReachAbove + PlacedItems::kFloorLift);
        REQUIRE(Vec3{item.transform[3]} == item.position);
    }
    REQUIRE(gems == 15);

    items.setPlayerCount(1);
    REQUIRE(items.visibleCount() >= 15);
    // The crystals turn: their tree's sequence loops, moving the gem's node.
    usize firstGem = 0;
    while (items.item(firstGem).name != "GEMORANGE") {
        ++firstGem;
    }
    REQUIRE(items.item(firstGem).player.playing());
    const Mat4 gemAtRest = items.item(firstGem).pose.matrices()[2];
    items.update(0.5f);
    const Mat4& turned = items.item(firstGem).pose.matrices()[2];
    REQUIRE(turned != gemAtRest);
    // About the vertical axis alone: up stays up, and the gem's foot stays put.
    REQUIRE(Vec3{turned[1]}.y == Approx(1.0f).margin(1e-4f));
    REQUIRE(std::abs(Vec3{turned[1]}.x) < 1e-4f);
    REQUIRE(std::abs(Vec3{turned[1]}.z) < 1e-4f);
    REQUIRE(Vec3{turned[3]} == Vec3{0.0f, 0.0f, 0.0f});
    // The sheen slides over the crystal: the archive's scroll on its texture has moved.
    constexpr u32 kSheenTexture = 181;
    REQUIRE(items.item(firstGem).model.textureOffset(kSheenTexture) != Vec2{0.0f, 0.0f});
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.size() >= usize{60}); // a shadow, the crystal, its shine and glow
    bool glowing = false;
    for (const auto& draw : device.draws) {
        glowing = glowing || draw.state.blend == BlendMode::Additive;
    }
    REQUIRE(glowing);

    const usize allDraws = device.draws.size();
    device.draws.clear();
    items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::DepthWriting);
    const usize solidDraws = device.draws.size();
    REQUIRE(solidDraws > 0);
    for (const auto& draw : device.draws) {
        CHECK(draw.state.depthWrite);
    }
    device.draws.clear();
    items.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::Effects);
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.state.depthTest);
    }
    CHECK(solidDraws + device.draws.size() == allDraws);

    items.setPlayerCount(0);
    device.draws.clear();
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty());
    items.clear();
    REQUIRE(items.size() == 0);
}

TEST_CASE("a collector on a crystal takes it and its burst plays", "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    const usize shown = items.visibleCount();
    usize gem = 0;
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "GEMORANGE") {
            gem = i;
            break;
        }
    }
    const PlacedItems::Item& crystal = items.item(gem);
    REQUIRE(crystal.realm() == 1); // the orange gems count towards the first realm
    REQUIRE(crystal.radius == 0.1f);
    REQUIRE(crystal.height == 2.0f);
    // Off to the side nothing is taken; a character's whole width away, the crystal goes to
    // one whose reach is that width, not to one reaching half of it.
    Collector far;
    far.position = crystal.position + Vec3{3.0f, 0.0f, 0.0f};
    REQUIRE(items.collect(device, std::array{far}).empty());
    Collector on;
    on.position = crystal.position + Vec3{1.4f, 1.0f, 0.0f};
    on.radius = 0.75f;
    REQUIRE(items.collect(device, std::array{on}).empty());
    on.radius = 1.5f;
    items.setOpener(gem, 2); // as though a chest had held it
    const std::vector<Pickup> pickups = items.collect(device, std::array{on});
    REQUIRE(pickups.size() == 1);
    REQUIRE(pickups[0].item == gem);
    CHECK(pickups[0].opener == 2);
    REQUIRE(pickups[0].collector == 0);
    REQUIRE(pickups[0].subtype == ItemInfo::kCrystal);
    REQUIRE(pickups[0].realm == 1);
    REQUIRE(pickups[0].position == crystal.position);
    REQUIRE(crystal.taken);
    REQUIRE_FALSE(crystal.visible);
    REQUIRE(items.visibleCount() == shown - 1);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.collect(device, std::array{on}).empty()); // only once
    items.setPlayerCount(2);
    REQUIRE_FALSE(crystal.visible); // taken stays taken
    // The burst's sparks pour out for the tree's second and a half, then die away.
    REQUIRE(items.burstParticleCount() == 0);
    items.update(1.0f / 30.0f);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.burstParticleCount() > 0);
    // Its emitters ride the tree's sequence: two of them, moving on as it plays.
    const PlacedItems::Effect& burst = items.effect(0);
    REQUIRE(burst.player.playing());
    REQUIRE(burst.emitters.size() == 2);
    const Mat4 emitterStart = items.bursts().emitter(burst.emitters[0].second).node();
    device.draws.clear();
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    bool sparks = false;
    for (const auto& draw : device.draws) {
        sparks = sparks || (draw.texture ==
                            &powerups.textures.texture(device, *powerups.textures.find("ORAN03")));
    }
    REQUIRE(sparks);
    items.update(1.0f);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.bursts().emitter(burst.emitters[0].second).node() != emitterStart);
    // Frame by frame (a long jump counts as one frame): the emitters stop at a second and a
    // half and the last sparks die within the next.
    for (s32 frame = 0; frame < 90; ++frame) {
        items.update(1.0f / 30.0f);
    }
    REQUIRE(items.effectCount() == 0);
    REQUIRE(items.burstParticleCount() == 0);
}

TEST_CASE("items can be dropped by their record's name and left lying or part taken",
          "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    const usize before = items.size();
    REQUIRE_FALSE(items.place(device, "NO_SUCH_THING", Vec3{0.0f}, nullptr));
    REQUIRE(items.place(device, "KEYRING", Vec3{500.0f, 0.0f, 500.0f}, nullptr));
    REQUIRE(items.place(device, "POT_GRE", Vec3{520.0f, 0.0f, 500.0f}, nullptr));
    REQUIRE(items.size() == before + 2);
    const PlacedItems::Item ring = items.item(before);
    REQUIRE(ring.visible);
    REQUIRE(ring.subtype == 2);
    REQUIRE(ring.value == 3);
    REQUIRE(items.item(before + 1).flags == 4); // the green potion's kind
    // By its record's number too, as a chest drops what it held, holding as many as said.
    const s32 keyRecord = items.item(before).info;
    REQUIRE_FALSE(items.placeRecord(device, -1, Vec3{0.0f}, nullptr));
    REQUIRE_FALSE(items.placeRecord(device, 9999, Vec3{0.0f}, nullptr));
    REQUIRE(items.placeRecord(device, keyRecord, Vec3{540.0f, 0.0f, 500.0f}, nullptr, 5));
    REQUIRE(items.item(before + 2).value == 5);
    REQUIRE(items.item(before + 2).name == "KEYRING");

    Collector on;
    on.position = Vec3{500.0f, 0.0f, 500.0f};
    const std::array<Collector, 1> party{on};
    // Refused, it stays; part taken, it stays with what is left; taken, it goes.
    s32 asked = 0;
    REQUIRE(items
                .collect(device, party,
                         [&](const Pickup& pickup) -> std::optional<s32> {
                             ++asked;
                             REQUIRE(pickup.amount == 3);
                             REQUIRE(pickup.subtype == 2);
                             return std::nullopt;
                         })
                .empty());
    REQUIRE(asked == 1);
    REQUIRE(items.item(before).visible);
    std::vector<Pickup> got =
        items.collect(device, party, [](const Pickup&) -> std::optional<s32> { return 1; });
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].amount == 3);
    REQUIRE(items.item(before).visible);
    REQUIRE(items.item(before).value == 1);
    got = items.collect(device, party, [](const Pickup& pickup) -> std::optional<s32> {
        REQUIRE(pickup.amount == 1);
        return 0;
    });
    REQUIRE(got.size() == 1);
    REQUIRE(items.item(before).taken);
    REQUIRE(items.collect(device, party).empty());
}

TEST_CASE("the crystals can start unseen and be revealed from the origin outward",
          "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    REQUIRE_FALSE(items.revealing());
    items.hideCrystals();
    REQUIRE(items.revealing());
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty()); // unseen
    const auto fading = [&]() {
        usize count = 0;
        for (usize i = 0; i < items.size(); ++i) {
            const PlacedItems::Item& item = items.item(i);
            count += item.subtype == ItemInfo::kCrystal && item.alpha > 0.0f ? 1 : 0;
        }
        return count;
    };
    // The reveal starts 26 units out; the nearest gem stands 38 out, so nothing shows yet.
    items.reveal(0.0f);
    REQUIRE(fading() == 0);
    // A second on it reaches 41: that gem alone fades, nearly whole within the second.
    items.reveal(1.0f);
    REQUIRE(fading() == 1);
    for (usize i = 0; i < items.size(); ++i) {
        const PlacedItems::Item& item = items.item(i);
        if (item.alpha > 0.0f) {
            REQUIRE(item.alpha > 0.9f);
            REQUIRE(item.alpha < 1.0f);
        }
    }
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(device.draws[0].state.depthWrite); // blended while it fades
    // Four seconds more reach past the farthest, and every gem is whole.
    items.reveal(4.0f);
    REQUIRE_FALSE(items.revealing());
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).subtype == ItemInfo::kCrystal) {
            REQUIRE(items.item(i).alpha == 1.0f);
        }
    }
    items.reveal(1.0f); // nothing left to reveal
    REQUIRE_FALSE(items.revealing());
}

TEST_CASE("thrown coins cross gaps and fast landings but are removed below the level",
          "[game][world][coin-flight]") {
    const auto dir = test::scratchDirectory("coin-flight");
    writeTextFile(dir / "coin.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json",
                  R"({"objects":[{"index":0,"name":"COIN","file":"coin.obj","meshTriangles":1}]})");
    writeFile(dir / "coin.png", test::kTinyPng);
    writeTextFile(
        dir / "textures.json",
        R"({"bitmaps":[{"index":0,"name":"COIN","file":"coin.png","width":2,"height":2}]})");
    writeTextFile(
        dir / "animations.json",
        R"({"trees":[{"name":"COIN","nodes":[{"object":"COIN","parent":-1,"position":[0,0,0]}]}]})");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":1,"subtype":6,"name":"COIN","value":100}],"itemInstances":[]})");
    WorldLayout layout;
    ItemArchive archive;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    REQUIRE(archive.load(dir));
    // The landing starts at z=2; nothing supports the flight before then.
    std::vector<CollisionTriangle> triangles(2);
    triangles[0].vertices = {Vec3{-20, 0, 2}, Vec3{20, 0, 30}, Vec3{20, 0, 2}};
    triangles[1].vertices = {Vec3{-20, 0, 2}, Vec3{-20, 0, 30}, Vec3{20, 0, 30}};
    WorldCollision collision;
    collision.build(triangles);
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    items.bind(device, layout, &collision, archives);
    items.setPlayerCount(1);
    REQUIRE(items.throwItem(device, "COIN", Vec3{0, 4, 0}, Vec3{0, -1, 10}, &collision, 0));
    REQUIRE(items.throwItem(device, "COIN", Vec3{0, 2, 5}, Vec3{0, -180, 0}, &collision, 0));
    REQUIRE(items.throwItem(device, "COIN", Vec3{50, 4, 0}, Vec3{0, -1, 0}, &collision, 0));
    items.update(1.0f / 30);
    CHECK(items.item(0).visible); // still airborne above the gap
    CHECK(items.item(1).visible);
    CHECK(items.item(1).position.y == Approx(PlacedItems::kThrownFloorLift));
    CHECK(items.item(1).velocity.y > 0); // swept landing bounces instead of tunnelling
    for (s32 frame = 0; frame < 300; ++frame) {
        items.update(1.0f / 30);
    }
    CHECK(items.item(0).visible);
    CHECK_FALSE(items.item(0).thrown);
    CHECK(items.item(0).position.y == Approx(PlacedItems::kThrownFloorLift));
    CHECK_FALSE(items.item(2).visible);
    CHECK(items.item(2).taken);
}

TEST_CASE("a thrown item sails out, bounces to rest on the floor and can be taken only after "
          "a while",
          "[game][world][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("ITEMS/LEVELG5/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELG5"));
    WorldCollision collision;
    REQUIRE(collision.load(root / "LEVELS/LEVELG5", layout));
    ItemArchive crypt;
    REQUIRE(crypt.load(root / "ITEMS/LEVELG5"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&crypt};
    items.bind(device, layout, &collision, archives);
    items.setPlayerCount(1);
    REQUIRE_FALSE(items.goldLeft());
    const usize index = items.size();
    // From three up over the boss's mark, thrown up and toward where the party comes in.
    REQUIRE(items.throwItem(device, "COIN_GOLD", Vec3{0.0f, 3.0f, 0.0f}, Vec3{0.0f, 20.0f, 10.0f},
                            &collision, 2.0f));
    REQUIRE(items.goldLeft());
    const PlacedItems::Item& coin = items.item(index);
    REQUIRE(coin.thrown);
    REQUIRE(coin.value == 5000);
    REQUIRE(coin.position == Vec3{0.0f, 3.0f, 0.0f}); // not on the floor yet
    REQUIRE_FALSE(coin.takeable());
    // These sprites spin through sequence-keyed texture frames, not geometry.
    coin.model.draw(device, Mat4{1}, coin.transform);
    REQUIRE_FALSE(device.draws.empty());
    const Texture* firstTexture = device.draws.back().texture;
    device.draws.clear();
    items.update(5.0f / 30.0f);
    coin.model.draw(device, Mat4{1}, coin.transform);
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(device.draws.back().texture != firstTexture);
    // It rises first, then falls and bounces.
    f32 highest = 0.0f;
    s32 bounces = 0;
    f32 lastVy = coin.velocity.y;
    for (s32 i = 0; i < 300; ++i) {
        items.update(1.0f / 60.0f);
        highest = std::max(highest, coin.position.y);
        if (lastVy < 0.0f && coin.velocity.y > 0.0f) {
            ++bounces;
        }
        lastVy = coin.velocity.y;
    }
    REQUIRE(highest > 5.0f);
    REQUIRE(bounces >= 1);
    CAPTURE(coin.position.x, coin.position.y, coin.position.z, coin.velocity.y, coin.velocity.z);
    REQUIRE_FALSE(coin.thrown);
    REQUIRE(coin.velocity == Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(coin.position.z > 2.0f);
    const auto floor = collision.floorAt(coin.position, PlacedItems::kFloorReachAbove,
                                         PlacedItems::kFloorReachBelow);
    REQUIRE(floor.has_value());
    REQUIRE(coin.position.y == Approx(floor->y + PlacedItems::kThrownFloorLift));
    REQUIRE(Vec3{coin.transform[3]} == coin.position);
    // Five seconds on it can be taken, and once it is no gold is left.
    REQUIRE(coin.takeable());
    Collector on;
    on.position = coin.position;
    const std::array<Collector, 1> party{on};
    const std::vector<Pickup> got = items.collect(device, party);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].amount == 5000);
    REQUIRE_FALSE(items.goldLeft());
    // With no floor to land on it stays where it is thrown.
    REQUIRE(items.throwItem(device, "COIN_BRONZE", Vec3{5.0f, 3.0f, 5.0f}, Vec3{10.0f, 20.0f, 0.0f},
                            nullptr, 0.0f));
    REQUIRE_FALSE(items.item(index + 1).thrown);
    REQUIRE(items.item(index + 1).takeable());
}

TEST_CASE("an instance's minimum can mean exactly that many players", "[game][world]") {
    PlacedItems::Item item;
    item.minPlayers = 2;
    REQUIRE_FALSE(item.shownTo(1));
    REQUIRE(item.shownTo(2));
    REQUIRE(item.shownTo(4));
    item.minPlayers = PlacedItems::kExactPlayersMark + 2;
    REQUIRE_FALSE(item.shownTo(1));
    REQUIRE(item.shownTo(2));
    REQUIRE_FALSE(item.shownTo(3));
}

TEST_CASE("bottles use authored health and armor rather than the food blast threshold",
          "[game][world][blast-items][shattered-potion]") {
    const auto dir = test::scratchDirectory("placed-bottle-damage");
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"BOTTLE","nodes":[
        {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}]}]})");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
        "itemInfos":[{"type":1,"subtype":4,"name":"BOTTLE","collisionType":1,
        "radius":0.5,"height":2,"armor":2,"hitPoints":3,"properties":2,
        "collisionOffset":[2,0,0]}],
        "itemInstances":[{"info":0,"position":[0,0,0],"minPlayers":1}]})");
    WorldLayout layout;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 100).empty()); // hidden instance
    items.setPlayerCount(1);
    CHECK(items.blast(device, Vec3{0}, 1, 100).empty()); // wrong collision center
    items.attach(0, Mat4{1}, true);
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 100).empty()); // held in chest
    items.attach(0, Mat4{1}, false);
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 0).empty());
    CHECK(items.item(0).health == 3);
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 2.1f).empty()); // rounds to zero
    CHECK(items.item(0).health == 3);
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 2).empty()); // armor floor: one
    CHECK(items.item(0).health == 2);
    const auto changes = items.blast(device, Vec3{2, 0, 0}, 1, 4);
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.front().potion == 2);
    CHECK(changes.front().position == Vec3{0});
    CHECK(items.item(0).health == 0);
    CHECK_FALSE(items.item(0).takeable());
    CHECK(items.blast(device, Vec3{2, 0, 0}, 1, 100).empty());
    REQUIRE(items.place(device, "BOTTLE", Vec3{0}, nullptr));
    CHECK(items.item(1).health == 3); // dropped bottles also initialize health
}

} // namespace
