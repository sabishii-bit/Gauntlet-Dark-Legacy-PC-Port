#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/characters/CharacterSave.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"
#include "game/tower/TowerRelics.h"

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

TEST_CASE("reloading a save replays only the relic ceremonies pending in that save",
          "[game][tower-relics][tower-relic-save]") {
    CharacterSave save;
    REQUIRE(save.progress().relics.addRune(0));
    const auto beforeCeremony = save.toJson();
    TowerRelics::acknowledge(save.progress().relics, {TowerRelics::Kind::Rune, 0});
    const auto afterCeremony = save.toJson();
    MessageTable strings;
    messages(strings);
    TowerRelics display;
    const auto reload = [&](const auto& serialized) {
        const auto loaded = CharacterSave::fromJson(serialized);
        const std::array party{loaded.progress().relics};
        display.begin(party, strings);
        CHECK(loaded.progress().relics.hasRune(0));
    };
    reload(beforeCeremony);
    REQUIRE(display.active());
    REQUIRE(display.current()->kind == TowerRelics::Kind::Rune);
    reload(afterCeremony);
    CHECK_FALSE(display.active());
    CHECK(display.displayedRunes() == 1);
    // Completing the ceremony changes the live save, not an older manual save.
    reload(beforeCeremony);
    CHECK(display.active());
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
    // The pane sounds its strike 91 ticks after the shard sets out (the window camera's
    // forty ticks and its count from 300 to 250), not when the flight is over.
    const auto flying = display.update(60, 1, false);
    REQUIRE_FALSE(flying.completed);
    REQUIRE(flying.sound.empty());
    const auto struck = display.update(60, 1, false);
    REQUIRE(struck.sound == "S_STNDGLASS");
    const auto complete = struck.completed;
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
    const auto fall = display.update(2000, 0, false);
    REQUIRE(fall.placement);
    REQUIRE(fall.sound == "S_RUNEFALL");
    // The stone lands 91 ticks into its fall, on the rune camera's count as the shard's pane
    // strikes on the window's, however long the effect itself runs.
    REQUIRE(display.update(90, 90.0f / 60, false).sound.empty());
    const auto landed = display.update(1, 1.0f / 60, false);
    REQUIRE(landed.sound == "S_RUNEHIT");
    REQUIRE_FALSE(landed.completed);
    const auto done = display.update(120, 2, false);
    REQUIRE(done.sound.empty());
    REQUIRE(done.completed.has_value());
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
        auto model = *effect.model;
        model.setAppearance({.unlit = effect.unlit,
                             .tint = effect.tint,
                             .depthWrite = effect.depthWrite,
                             .additive = effect.additive});
        model.setFrame(effect.player.sequence(), static_cast<s32>(effect.player.frame()));
        if (const auto* bank = display.figures().textureAnimator(effect.archive, effect.lenders)) {
            bank->apply(model, *effect.tree, effect.player.sequence(), effect.player.frame());
        }
        model.draw(device, Mat4{1}, effect.transform(), world.lighting(), effect.pose.matrices());
        REQUIRE_FALSE(device.draws.empty());
        if (i < 12) {
            INFO(i);
            // TEXFADEIN reveals a flattened ground shadow, not an opaque shell.
            // Its parent keys Y scale to .001; check the posed geometry rather
            // than hiding the entire mesh by reversing the authored fade.
            auto shadow = model;
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
    GameServices context;
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
    GameServices context;
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
        GameServices context;
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
TEST_CASE("native tower relic rewind crosses speech placement and wizard resets",
          "[local-rewind][tower-relics-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    std::array<Relics, 1> party;
    party[0].addRune(0);
    party[0].addShard(1);
    MessageTable strings;
    messages(strings);
    TowerRelics actual;
    TowerRelics expected;
    TowerRelics foreign;
    for (auto* relics : {&actual, &expected}) {
        relics->begin(party, strings);
        relics->bind(device, world, {});
    }
    const auto draw = [&](const TowerRelics& relics) {
        device.draws.clear();
        relics.draw(device, Mat4{1}, world.lighting(), {}, 0.37f);
        relics.drawWizard(device, Mat4{1}, world.lighting(), {}, 0.37f);
        return device.draws;
    };
    TowerRelics::LocalSnapshot saved;
    LevelWorld::LocalSnapshot worldSaved;
    CHECK_FALSE(actual.restoreLocal(saved));
    s32 completions = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window);
        const s32 distance = window % 8 + 1;
        actual.saveLocal(saved);
        world.saveLocal(worldSaved);
        CHECK_FALSE(foreign.restoreLocal(saved));
        const auto before = draw(actual);
        for (s32 tick = 0; tick < distance; ++tick) {
            actual.capturePresentation();
            actual.update(127, 0.4f, false);
            actual.animate(0.4f);
        }
        REQUIRE(world.restoreLocal(worldSaved));
        REQUIRE(actual.restoreLocal(saved));
        const auto restored = draw(actual);
        REQUIRE(restored.size() == before.size());
        for (usize i = 0; i < before.size(); ++i) {
            test::checkRecordedDraw(restored[i], before[i]);
        }
        for (s32 tick = 0; tick < distance; ++tick) {
            actual.capturePresentation();
            expected.capturePresentation();
            const auto a = actual.update(10, 1.0f / 30, false);
            const auto b = expected.update(10, 1.0f / 30, false);
            CHECK(a.voice == b.voice);
            CHECK(a.sound == b.sound);
            CHECK(a.placement == b.placement);
            REQUIRE(a.completed.has_value() == b.completed.has_value());
            if (a.completed) {
                ++completions;
                CHECK(a.completed->kind == b.completed->kind);
                CHECK(a.completed->index == b.completed->index);
            }
            actual.animate(1.0f / 30);
            expected.animate(1.0f / 30);
            CHECK(actual.phase() == expected.phase());
            CHECK(actual.caption() == expected.caption());
            CHECK(actual.captionPage() == expected.captionPage());
            CHECK(actual.captionLetters() == expected.captionLetters());
            CHECK(actual.displayedRunes() == expected.displayedRunes());
            CHECK(actual.displayedShards() == expected.displayedShards());
        }
        const auto reference = draw(expected);
        const auto corrected = draw(actual);
        REQUIRE(corrected.size() == reference.size());
        for (usize i = 0; i < reference.size(); ++i) {
            test::checkRecordedDraw(corrected[i], reference[i]);
        }
    }
    CHECK(completions >= 2);
    CHECK_FALSE(actual.active());
    CHECK(device.textureUpdates == 0);
    actual.clear();
    CHECK_FALSE(actual.restoreLocal(saved));
    actual.begin(party, strings);
    actual.saveLocal(saved);
    actual.bind(device, world, {});
    CHECK_FALSE(actual.restoreLocal(saved));
}

TEST_CASE("tower relic digest bytes stay pinned after the ceremony script and what it saved",
          "[game][tower-relics][digest-golden]") {
    const auto root = test::sampleLevel("tower-relics-digest-golden");
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, LevelRef{"TEST", 0, "TEST", "", "", "", "", 0}));
    std::array<Relics, 1> party;
    party[0].addRune(0);
    party[0].addShard(1);
    MessageTable strings;
    messages(strings);
    TowerRelics relics;
    relics.begin(party, strings);
    relics.bind(device, world, {});
    const auto step = [&relics] {
        relics.capturePresentation();
        (void)relics.update(10, 1.0f / 30, false);
        relics.animate(1.0f / 30);
    };
    for (s32 tick = 0; tick < 4; ++tick) {
        step();
    }
    TowerRelics::LocalSnapshot saved;
    relics.saveLocal(saved);
    for (s32 tick = 4; tick < 12; ++tick) {
        step();
    }
    NativeStateResources resources;
    test::registerStage(resources, world, device);
    REQUIRE(relics.registerNativeResources(resources));
    // SHA-256 of the ceremony's native digest twelve updates in and of what it saved four
    // updates in, which netplay peers must hash alike: a literal moves only when what is visited
    // changes on purpose.
    test::checkPinnedDigests("TowerRelics", test::liveDigest(relics, resources.resources()),
                             test::savedDigest(relics, saved, resources.resources()),
                             "7b134812a517fcaab09c4816ffc15a5af551f403a3cc0f029464dd9c06f5197e",
                             "2602812209950d12d7898d6e21f4731e05e1f7741178782672aaa685fcee4bcb");
}
} // namespace
