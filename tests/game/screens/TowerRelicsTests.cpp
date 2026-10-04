#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/players/CharacterSave.h"
#include "game/screens/TowerRelics.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void messages(MessageTable& table) {
    const auto file = test::scratchDirectory("tower-relics") / "text.json";
    writeTextFile(file, R"({"messages":[
        {"name":"NEWSHARDS","lines":["Unused","Town glass","Mountain glass"]},
        {"name":"NEWRUNES","lines":["Runestone"]},
        {"name":"MORESHARDS","lines":["Continue","More shards"]}]})");
    REQUIRE(table.load(file));
}

TEST_CASE("tower acquisitions are persisted independently of collection ownership",
          "[game][tower-relics]") {
    CharacterSave save;
    save.name = "TEST";
    auto& relics = save.progress().relics;
    REQUIRE(relics.addRune(0));
    REQUIRE(relics.addShard(1));
    const auto reloaded = CharacterSave::fromJson(save.toJson());
    REQUIRE(reloaded.progress().relics == relics);
    REQUIRE(relics.pendingRunes == 1);
    REQUIRE(relics.pendingShards == 2);
    TowerRelics::acknowledge(relics, {TowerRelics::Kind::Rune, 0});
    REQUIRE(relics.hasRune(0));
    REQUIRE(relics.pendingRunes == 0);
    REQUIRE_FALSE(relics.addRune(0));
    REQUIRE(relics.pendingRunes == 0);
    REQUIRE(relics.pendingShards == 2);
    const auto legacy = CharacterSave::fromJson(R"({"name":"OLD","character":0,
        "classes":{"WAR":{"relics":{"runes":8191,"shards":510}}}})");
    REQUIRE(legacy.progress().relics.runes == 8191);
    REQUIRE(legacy.progress().relics.pendingRunes == 0);
    REQUIRE(legacy.progress().relics.pendingShards == 0);
}

TEST_CASE("tower collection uses party union without replaying an already installed piece",
          "[game][tower-relics]") {
    std::array<Relics, 2> party;
    party[0].addRune(3);
    party[0].addShard(2);
    party[1].runes = party[0].runes;
    party[1].shards = party[0].shards;
    TowerRelics display;
    MessageTable strings;
    messages(strings);
    display.begin(party, strings);
    REQUIRE_FALSE(display.active());
    REQUIRE(display.displayedRunes() == 8);
    REQUIRE(display.displayedShards() == 4);
    party[0].addRune(12);
    display.begin(party, strings);
    REQUIRE(display.current()->tree() == "RUNE13");
    REQUIRE(display.current()->anchor() == "L1RUNE13");
    REQUIRE(display.current()->camera() == 203);
    display.clear();
    REQUIRE_FALSE(display.active());
    REQUIRE_FALSE(display.camera());
}

TEST_CASE("tower ceremonies hold for speech then placement and complete once in order",
          "[game][tower-relics]") {
    std::array<Relics, 1> party;
    party[0].addRune(0);
    party[0].addShard(1);
    TowerRelics display;
    MessageTable strings;
    messages(strings);
    display.begin(party, strings);
    REQUIRE(display.current()->tree() == "SHARD1");
    REQUIRE(display.current()->anchor() == "L1WINDOWFRAME");
    REQUIRE(display.update(119, 0, false).voice.empty());
    REQUIRE(display.update(1, 0, false).voice == "S_SHRD4TWN");
    REQUIRE_FALSE(display.update(2000, 0, true).placement);
    REQUIRE(display.phase() == TowerRelics::Phase::Speech);
    REQUIRE(display.update(1, 0, false).placement);
    REQUIRE(display.phase() == TowerRelics::Phase::Placement);
    REQUIRE_FALSE(display.update(60, 1, false).completed);
    const auto complete = display.update(60, 1, false).completed;
    REQUIRE(complete.has_value());
    REQUIRE(complete->kind == TowerRelics::Kind::Shard);
    REQUIRE(display.displayedShards() == 2);
    REQUIRE(display.current()->kind == TowerRelics::Kind::Followup);
    REQUIRE(display.update(120, 0, false).voice == "S_CONTINUEVOX");
    REQUIRE_FALSE(display.update(2000, 0, false).completed);
    REQUIRE(display.captionPage() == 1);
    REQUIRE(display.update(2000, 0, false).completed.has_value());
    REQUIRE(display.current()->tree() == "RUNE1");
    REQUIRE(display.update(120, 2, false).voice == "S_FNDRUNEYOU");
    REQUIRE(display.update(2000, 0, false).placement);
    REQUIRE(display.update(120, 2, false).completed.has_value());
    REQUIRE_FALSE(display.active());
    REQUIRE_FALSE(display.update(120, 2, false).completed);
    REQUIRE(display.displayedRunes() == 1);
}

TEST_CASE("tower scenarios distinguish displayed collections from new finds",
          "[game][tower-relics]") {
    const auto scenario = Scenario::fromJson(R"({"party":[{"class":"WAR","level":20,
        "runes":[1,2],"shards":[2],"newRunes":[0],"newShards":[1]}]})");
    const auto party = scenario.partyMembers();
    const auto& relics = party[0].save.progress().relics;
    REQUIRE(relics.runes == 7);
    REQUIRE(relics.shards == 6);
    REQUIRE(relics.pendingRunes == 1);
    REQUIRE(relics.pendingShards == 2);
    REQUIRE_THROWS(Scenario::fromJson(R"({"party":[{"runes":[13]}]})"));
    REQUIRE_THROWS(Scenario::fromJson(R"({"party":[{"newShards":[0]}]})"));
}

TEST_CASE("all authored tower pieces retain settled meshes at their own world anchors",
          "[game][tower-relics][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    std::array<Relics, 1> party;
    party[0].runes = 0x1fff;
    party[0].shards = 0x1fe;
    TowerRelics display;
    MessageTable strings;
    messages(strings);
    display.begin(party, strings);
    display.bind(device, world, Vec3{0});
    REQUIRE_FALSE(display.active());
    REQUIRE(display.figures().count() == 21);
    usize garmLifts = 0;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.id == 255) {
            REQUIRE(trigger.forced);
            REQUIRE(world.triggers().opened(trigger.target));
            REQUIRE(world.triggers().settled(trigger.target));
            ++garmLifts;
        }
    }
    REQUIRE(garmLifts > 0);
    for (usize i = 0; i < display.figures().count(); ++i) {
        const auto& effect = display.figures().effect(i);
        REQUIRE(effect.player.frame() == static_cast<f32>(effect.player.frameCount() - 1));
        REQUIRE(effect.particles.field().size() == 0);
        std::string_view anchor = "L1WINDOWFRAME";
        if (i <= 12) {
            anchor = i == 12 ? "L1RUNE13" : "L1RUNEPLACE";
        }
        bool found = false;
        for (usize j = 0; j < world.layout().objects().size(); ++j) {
            if (world.layout().objects()[j].name == anchor) {
                REQUIRE(effect.position == Vec3{world.scene().worldTransform(j)[3]});
                found = true;
            }
        }
        REQUIRE(found);
        device.draws.clear();
        effect.model.draw(device, Mat4{1}, effect.transform(), world.lighting(),
                          effect.pose.matrices());
        REQUIRE_FALSE(device.draws.empty());
        if (i < 12) {
            INFO(i);
            // TEXFADEIN reveals a flattened ground shadow, not an opaque shell.
            // Its parent keys Y scale to .001; check the posed geometry rather
            // than hiding the entire mesh by reversing the authored fade.
            auto shadow = effect.model;
            for (usize node = 0; node < effect.tree->nodes.size(); ++node) {
                const auto& info = effect.tree->nodes[node];
                bool faded = false;
                for (s32 ancestor = info.parent; ancestor >= 0;
                     ancestor = effect.tree->nodes[static_cast<usize>(ancestor)].parent) {
                    const s32 mod =
                        effect.tree->nodes[static_cast<usize>(ancestor)].textureAnimation;
                    if (mod < 0 ||
                        !world.items().trees.textureAnimations()[static_cast<usize>(mod)].fades()) {
                        continue;
                    }
                    faded = true;
                    break;
                }
                if (!faded) {
                    shadow.setMeshAlpha(node, 0);
                }
            }
            device.draws.clear();
            shadow.draw(device, Mat4{1}, effect.transform(), world.lighting(),
                        effect.pose.matrices());
            REQUIRE_FALSE(device.draws.empty());
            for (const auto& draw : device.draws) {
                f32 low = draw.vertices.front().position.y;
                f32 high = low;
                for (const auto& vertex : draw.vertices) {
                    low = std::min(low, vertex.position.y);
                    high = std::max(high, vertex.position.y);
                }
                INFO(low);
                INFO(high);
                REQUIRE(high - low < 0.02f);
            }
        }
    }
    display.animate(1000);
    REQUIRE(display.figures().count() == 21);
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == "L1XPLIGHTRAY01") {
            REQUIRE(world.objectAlpha(i) == 1);
        }
    }
    display.clear();
    REQUIRE(display.figures().count() == 0);
}

TEST_CASE("tower return routes relic ceremonies before releasing player controls",
          "[game][tower-relics][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const auto scenario = Scenario::fromJson(R"({"welcome":false,"arrivalWorld":7,
        "party":[{"class":"WAR","level":20,"newRunes":[0],"newShards":[1]}]})");
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    REQUIRE(scene.towerRelics().active());
    const Vec3 position = scene.actor(0)->position();
    PlayScene::Inputs inputs{};
    inputs[0].move = MoveInput{Vec2{1, 0}, 1};
    s32 frames = 0;
    while (scene.towerRelics().active() && frames++ < 1800) {
        scene.update(1.0 / 30, inputs);
        REQUIRE(scene.actor(0)->position() == position);
    }
    REQUIRE(frames < 1800);
    const auto savedParty = scene.party();
    REQUIRE(savedParty[0].save.progress().relics.pendingRunes == 0);
    REQUIRE(savedParty[0].save.progress().relics.pendingShards == 0);
    REQUIRE(scene.towerRelics().figures().count() == 2);
    scene.close();
    REQUIRE(scene.open(device, context, world, savedParty, scenario.tower));
    REQUIRE_FALSE(scene.towerRelics().active());
    REQUIRE(scene.towerRelics().figures().count() == 2);
    scene.close();
}

TEST_CASE("fully unlocked tower carries Garm's usable portal up with its revealed pedestal",
          "[game][tower-relics][portals][garm-portal][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/tower-fully-unlocked.json");
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    usize index = 0;
    while (index < scene.portals().size() && scene.portals().portal(index).tag != "h4") {
        ++index;
    }
    REQUIRE(index < scene.portals().size());
    const auto& portal = scene.portals().portal(index);
    REQUIRE(portal.destination);
    CHECK(portal.destination->name == "H4");
    REQUIRE_FALSE(portal.shut);
    REQUIRE(portal.model.bound());
    REQUIRE(portal.alpha == 1);
    REQUIRE(portal.support >= 0);
    CHECK(world.layout().objects()[static_cast<usize>(portal.support)].name == "L1ELEV669");
    const auto& authored = world.layout().itemInstances()[static_cast<usize>(portal.instance)];
    CHECK(portal.position.y > authored.position.y + 5);
    const auto floor = world.collision().floorAt(portal.position, 0.5f, 1);
    REQUIRE(floor);
    CHECK(floor->object == portal.support);
    CHECK(portal.position.y == Catch::Approx(floor->y + ExitPortals::kFloorLift));
    CHECK(Vec3{portal.transform[3]} == portal.position);
    device.draws.clear();
    portal.model.draw(device, Mat4{1}, portal.transform, world.lighting(), portal.pose.matrices());
    CHECK_FALSE(device.draws.empty());

    // The actual scene's relocated portal must accept visitors at its visible height,
    // not the old authored spot nine units below the raised platform.
    const std::array visitor{PortalVisitor{portal.position, 0.75f}};
    CHECK(scene.portals().flamePosition(visitor) == portal.position);
    const std::array underground{PortalVisitor{authored.position, 0.75f}};
    CHECK_FALSE(scene.portals().flamePosition(underground));
    scene.close();
}
TEST_CASE("tower relic ceremony draws fractional motion without consuming its events",
          "[game][tower-relics][assets][presentation]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    std::array<Relics, 1> party;
    party[0].addRune(0);
    MessageTable strings;
    messages(strings);
    TowerRelics display;
    display.begin(party, strings);
    display.bind(device, world, {});
    REQUIRE(display.update(120, 0, false).voice == "S_FNDRUNEYOU");
    REQUIRE(display.update(2000, 0, false).placement);
    REQUIRE(display.figures().count() == 1);
    display.animate(0.5f);
    display.animate(1.0f / 30);
    const auto& effect = display.figures().effect(0);
    REQUIRE(effect.presentationCaptured);
    const auto lived = effect.lived;
    const auto frame = effect.player.frame();
    const auto generation = effect.player.generation();
    for (const f32 blend : {0.0f, 0.25f, 0.75f, 1.0f, -1.0f}) {
        device.draws.clear();
        display.draw(device, Mat4{1}, world.lighting(), {}, blend);
        CHECK_FALSE(device.draws.empty());
        CHECK(effect.lived == lived);
        CHECK(effect.player.frame() == frame);
        CHECK(effect.player.generation() == generation);
        CHECK(display.phase() == TowerRelics::Phase::Placement);
        CHECK(display.displayedRunes() == 0);
    }
    display.capturePresentation();
    CHECK(effect.previousFrame == effect.player.presentationFrame());
    device.draws.clear();
    display.draw(device, Mat4{1}, world.lighting(), {}, 0);
    REQUIRE_FALSE(device.draws.empty());
    const auto held = device.draws.front().vertices.front().position;
    device.draws.clear();
    display.draw(device, Mat4{1}, world.lighting(), {}, 0.75f);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().vertices.front().position == held);
    CHECK_FALSE(display.update(0, 0, false).completed);
    display.clear();
    device.draws.clear();
    display.draw(device, Mat4{1}, world.lighting(), {}, 0.5f);
    display.drawWizard(device, Mat4{1}, world.lighting(), {}, 0.5f);
    CHECK(device.draws.empty());
}
TEST_CASE("Garm victory returns above the lowered battlefield bridge",
          "[game][garm-return][tower-relics][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/after-garm-victory.json");
    auto party = scenario.partyMembers();
    REQUIRE_FALSE(party.empty());
    // Only the just-completed boss is added by the ending flow. Its fixture must
    // already carry the prerequisites earned on the way to Garm.
    party[0].save.progress().levels.recordBeaten(8, 3);
    REQUIRE(TowerAccess(party).liftsOpen());

    for (const bool preadvanced : {false, true}) {
        CAPTURE(preadvanced);
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root));
        // Character saving shares and animates this tower before gameplay resumes.
        if (preadvanced) {
            for (s32 tick = 0; tick < 1800; ++tick) {
                world.update(1.0f / 60);
            }
        }
        LevelCatalog levels;
        REQUIRE(levels.load(root));
        const GameConfig config;
        GameContext context;
        context.config = &config;
        context.levels = &levels;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = false;
        options.arriving = true;
        options.arrivalWorld = 8;
        PlayScene scene;
        REQUIRE(scene.open(device, context, world, party, options));
        const auto* actor = scene.actor(0);
        REQUIRE(actor);
        const auto* marker = world.startPoint(11);
        REQUIRE(marker);
        CHECK(marker->position == Vec3{2.953125f, -28.40625f, 37.773438f});
        CHECK(actor->position().x == Catch::Approx(marker->position.x));
        CHECK(actor->position().z == Catch::Approx(marker->position.z));

        // The collision-only floor at the marker also exists with the bridge closed.
        // Check actual drawn geometry, which used to remain 16 units above the player.
        ModelSet models;
        REQUIRE(models.load(root / LevelRef::tower().directory));
        constexpr usize kBridgeFloor = 2568;
        REQUIRE(world.layout().objects()[kBridgeFloor].name == "L1FLOOR_LINE656");
        const auto model = models.find(world.layout().objects()[kBridgeFloor].name);
        REQUIRE(model);
        const auto& mesh = models.mesh(*model);
        REQUIRE_FALSE(mesh.vertices.empty());
        const Mat4 transform = world.scene().worldTransform(kBridgeFloor);
        for (const auto& vertex : mesh.vertices) {
            const Vec3 drawn = Vec3{transform * Vec4{vertex.position, 1}};
            CHECK(std::abs(drawn.y - actor->position().y) < 0.5f);
        }
        std::vector<WallContact> contacts;
        const Vec3 initial = actor->position();
        const Vec3 corrected = world.collision().resolveWalls(
            initial, actor->radius(), initial.y + 0.5f, initial.y + actor->height(), &contacts);
        CHECK(glm::distance(initial, corrected) < 0.001f);
        CHECK(contacts.empty());
        for (s32 tick = 0; tick < 600; ++tick) {
            scene.update(1.0 / 30, {});
        }
        const Vec3 settled = actor->position();
        CHECK(glm::distance(initial, settled) < 0.5f);
        PlayScene::Inputs input{};
        input[0].move = MoveInput{Vec2{0, 1}, 1};
        for (s32 tick = 0; tick < 90; ++tick) {
            scene.update(1.0 / 30, input);
        }
        CHECK(glm::distance(actor->position(), settled) > 5);
        scene.close();
    }
}
} // namespace
