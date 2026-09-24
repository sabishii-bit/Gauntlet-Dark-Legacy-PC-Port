#include <array>

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
        {"name":"NEWRUNES","lines":["Runestone"]}]})");
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
          "[game][tower-relics][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
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
            // Each stone has a black duplicate shell under an authored fade-out
            // node. Once installed, that shell must not draw over the stone.
            for (const auto& draw : device.draws) {
                REQUIRE(draw.texture != &world.items().textures.texture(device, 122));
            }
        }
    }
    display.animate(1000);
    REQUIRE(display.figures().count() == 21);
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == "L1XPLOWERLIGHTR") {
            REQUIRE(world.objectAlpha(i) == 1);
        }
    }
    display.clear();
    REQUIRE(display.figures().count() == 0);
}

TEST_CASE("tower return routes relic ceremonies before releasing player controls",
          "[game][tower-relics][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
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
} // namespace
