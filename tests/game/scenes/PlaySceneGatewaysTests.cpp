#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/characters/Progression.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/ImportedLevel.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/GatewayTower.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/HintMenu.h"
#include "game/tower/TowerGateways.h"
#include "game/widgets/OptionMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::garmBeaten;
using test::GatewayTower;

/** Where a party stands to be greeted by Dark Legacy's Sumner and handed his scroll. */
constexpr Vec3 kSumnerSpot{3.3f, 2.1f, -49.0f};

/** The first seat's menu keys for one update, the others idle. */
PlayScene::Inputs keys(bool down, bool select, bool back) {
    PlayScene::Inputs inputs{};
    inputs[0].menu.down = down;
    inputs[0].menu.select = select;
    inputs[0].menu.back = back;
    return inputs;
}
const PlayScene::Inputs kDown = keys(true, false, false);
const PlayScene::Inputs kSelect = keys(false, true, false);
const PlayScene::Inputs kBack = keys(false, false, true);

/** A character back from Garm whom Sumner has told of his gateways, so no scroll waits. */
CharacterSave toldOfGateways() {
    CharacterSave save = garmBeaten("AB");
    save.gatewaysAnnounced = true;
    return save;
}

/** Opens the tower with `party` at Sumner's spot, with his gateways (offline) or without (as
 * online), and waits for his greeting to hand the first seat his scroll. */
void visitSumner(GatewayTower& tower, std::span<const PartyMember> party, f64 step,
                 bool offline = true) {
    PlayOptions options;
    options.welcome = false;
    options.position = kSumnerSpot;
    tower.arrive(party, step, options, offline);
    for (s32 i = 0; i < 600 && !tower.scene.hints().active(); ++i) {
        tower.scene.update(step, {});
    }
    REQUIRE(tower.scene.hints().active());
    REQUIRE(tower.scene.hints().topics().isOpen());
}

/** The labels of a menu's rows and whether each can be chosen. */
std::vector<std::string> labels(const OptionMenu& menu) {
    std::vector<std::string> out;
    for (const MenuItem& item : menu.definition().items) {
        out.push_back(item.text);
    }
    return out;
}
std::vector<bool> choosable(const OptionMenu& menu) {
    std::vector<bool> out;
    for (const MenuItem& item : menu.definition().items) {
        out.push_back(item.enabled);
    }
    return out;
}

/** Every row of `menu` stands whole on its scroll, and the scroll on the screen. */
void checkOnScroll(const OptionMenu& menu) {
    const Rect scroll = menu.backdropArea();
    CHECK(scroll.x >= 0.0f);
    CHECK(scroll.x + scroll.width <= 512.0f);
    for (usize i = 0; i < menu.definition().items.size(); ++i) {
        CAPTURE(i, menu.definition().items[i].text);
        const Rect row = menu.itemArea(i);
        CHECK(row.width > 0.0f);
        CHECK(row.x >= scroll.x);
        CHECK(row.x + row.width <= scroll.x + scroll.width);
        CHECK(row.y >= scroll.y);
        CHECK(row.y + row.height <= scroll.y + scroll.height);
    }
}

const std::vector<std::string> kPlaces{"The Dark Legacy", "Legend of What Once Was",
                                       "Legend of What Could Have Been"};

} // namespace

TEST_CASE("past Garm, Dark Legacy's Sumner lists where his gateways lead, greying the unopened",
          "[game][screens][gateways][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    GatewayTower tower;
    PlayScene& scene = tower.scene;
    visitSumner(tower, std::vector{PartyMember{0, toldOfGateways()}}, step);
    // His four topics, then Legacy of Legends, written whole on his scroll; nothing is looked
    // for on disk until it is asked.
    const OptionMenu& topics = scene.hints().topics();
    REQUIRE(topics.definition().items.size() == 5);
    CHECK(topics.definition().items[4].text == "Legacy of Legends");
    CHECK(topics.definition().items[4].code == kGatewayTopic);
    CHECK(topics.definition().items[4].enabled);
    // FONT32 at the topics' scale measures it 327 pixels: from x 128 it ends at 455.
    CHECK(topics.definition().scale == 1.0f);
    CHECK(topics.itemArea(4).x == static_cast<f32>(HintMenu::kTopicsX));
    CHECK(topics.itemArea(4).width == 327.0f);
    checkOnScroll(topics);
    CHECK(tower.gateways.asks == 0);
    for (s32 i = 0; i < 4; ++i) {
        scene.update(step, kDown);
    }
    REQUIRE(topics.selection() == 4);
    // Asked with neither campaign installed, then each, then both: the same three places in the
    // same order, the party's own tower greyed with every world this installation cannot open.
    for (const auto& [n64, dreamcast] : std::array{std::pair{false, false}, std::pair{true, false},
                                                   std::pair{false, true}, std::pair{true, true}}) {
        CAPTURE(n64, dreamcast);
        tower.gateways.offered[0].usable = n64;
        tower.gateways.offered[1].usable = dreamcast;
        scene.update(step, kSelect);
        REQUIRE(scene.hints().reading());
        const OptionMenu& list = scene.hints().page();
        CHECK(labels(list) == kPlaces);
        CHECK(choosable(list) == std::vector<bool>{false, n64, dreamcast});
        // The widest, Legend of What Could Have Been, measures 586 pixels at the topics' scale,
        // so the rows shrink together until it keeps within the room.
        CHECK(list.definition().scale == static_cast<f32>(HintMenu::kChoicesRoom) / 586.0f);
        CHECK(list.itemArea(2).width <= static_cast<f32>(HintMenu::kChoicesRoom));
        checkOnScroll(list);
        if (!n64 && !dreamcast) {
            scene.update(step, kSelect); // nothing to choose: the list stays
            CHECK(scene.hints().reading());
        }
        // Back goes to his topics where they were left.
        scene.update(step, kBack);
        REQUIRE(scene.hints().active());
        REQUIRE_FALSE(scene.hints().reading());
        CHECK(topics.selection() == 4);
    }
    CHECK(tower.gateways.asks == 4); // looked for afresh each time the list is asked for
    CHECK_FALSE(scene.gatewayRequest());
    // A world chosen is the party's request, and the scroll burns away.
    scene.update(step, kSelect);
    REQUIRE(scene.hints().reading());
    CHECK(scene.hints().page().selection() == 1);
    scene.update(step, kDown);
    CHECK(scene.hints().page().selection() == 2);
    scene.update(step, kSelect);
    REQUIRE(scene.gatewayRequest());
    CHECK(*scene.gatewayRequest() == GatewayDestination{1});
    CHECK(scene.hints().burning());
    for (s32 i = 0; i < 300 && scene.hints().active(); ++i) {
        scene.update(step, {});
    }
    CHECK_FALSE(scene.hints().active());
    CHECK(*scene.gatewayRequest() == GatewayDestination{1});
    scene.close();
    CHECK_FALSE(scene.gatewayRequest());
}

TEST_CASE("Dark Legacy's Sumner offers no gateways before Garm falls, or online",
          "[game][screens][gateways][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    GatewayTower tower;
    CharacterSave unbeaten;
    unbeaten.name = "CD";
    unbeaten.progress().experience = levelExperience(40);
    visitSumner(tower, std::vector{PartyMember{0, unbeaten}}, step);
    CHECK(tower.scene.hints().topics().definition().items.size() == 4);
    visitSumner(tower, std::vector{PartyMember{0, toldOfGateways()}}, step, false);
    CHECK(tower.scene.hints().topics().definition().items.size() == 4);
    CHECK(tower.gateways.asks == 0);
    // One member past Garm is enough: the whole party would go.
    visitSumner(tower, std::vector{PartyMember{0, unbeaten}, PartyMember{1, toldOfGateways()}},
                step);
    CHECK(tower.scene.hints().topics().definition().items.size() == 5);
    tower.scene.close();
}

namespace {

/** A tower's resident past the gateways, as an expansion hosts one: the topics it is asked
 * for, with the gateway row where it says, and what the scene hands back to it. */
class GatewayResident final : public ImportedLevel {
public:
    std::optional<ImportedDialogue> offered;
    std::vector<s32> answered;
    s32 dismissals = 0;

    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::optional<ImportedDialogue> takeDialogue() override {
        return std::exchange(offered, std::nullopt);
    }
    std::vector<std::string> answerDialogue(s32 topic) override {
        answered.push_back(topic);
        return {};
    }
    void dismissDialogue() override { ++dismissals; }
};

/** The second world's tower, its resident's topics ending with an "exit" set apart, before
 * which it offers the gateways. */
ImportedDialogue residentTopics() {
    ImportedDialogue dialogue{
        0,
        {},
        {{0, "hints.general"}, {1, "hints.guardians"}, {4, "legends.dreamcast.sumner.topic.exit"}}};
    dialogue.gateways = ImportedDialogue::Gateways{2, 1};
    return dialogue;
}

} // namespace

TEST_CASE("a tower past the gateways lists them where its resident says, greying its own world",
          "[game][screens][gateways][imported-play][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    const AssetLocator assets(root);
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    LevelWorld world;
    auto resident = std::make_shared<GatewayResident>();
    LevelRef ref;
    ref.realm = "Legends";
    ref.name = "test:tower";
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
         {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    auto layout = WorldLayout::fromDecoded(
        {}, {{.kind = LocatorKind::Start, .position = {0, 0, 0}}}, {-100, 0, -100}, {100, 40, 100});
    REQUIRE(
        world.loadImported(device, root, ref, std::move(layout), std::move(collision), resident));
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.unpackedRoot = root;
    test::FixedGateways gateways;
    gateways.offered = {{0, "legends.n64.gateway.label", true},
                        {1, "legends.dreamcast.gateway.label", true}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    // Opened offline or as online, with the party in play, until its resident's menu is up.
    const auto talk = [&](bool offline) {
        scene.close();
        REQUIRE(scene.open(device, context, world, party, options,
                           offline ? PlayScene::Links{.gateways = &gateways} : PlayScene::Links{}));
        REQUIRE(scene.animator(0) != nullptr);
        for (s32 i = 0; i < 600 && (scene.spawning() || scene.animator(0)->entering()); ++i) {
            scene.update(step, {});
        }
        resident->offered = residentTopics();
        for (s32 i = 0; i < 10 && !scene.hints().active(); ++i) {
            scene.update(step, {});
        }
        REQUIRE(scene.hints().active());
    };
    talk(true);
    const OptionMenu& topics = scene.hints().topics();
    CHECK(labels(topics) ==
          std::vector<std::string>{"General Hints", "Guardians", "Legacy of Legends", "exit"});
    CHECK(topics.definition().items[2].code == kGatewayTopic);
    scene.update(step, kDown);
    scene.update(step, kDown);
    REQUIRE(topics.selection() == 2);
    scene.update(step, kSelect);
    // The same list, the resident's own world greyed and the party's own tower open to it.
    REQUIRE(scene.hints().reading());
    CHECK(labels(scene.hints().page()) == kPlaces);
    CHECK(choosable(scene.hints().page()) == std::vector<bool>{true, true, false});
    CHECK(resident->answered.empty()); // the row is the scene's, never the resident's
    // Back to its topics, then the way home chosen: the menu goes and the resident is told so.
    scene.update(step, kBack);
    REQUIRE_FALSE(scene.hints().reading());
    CHECK(topics.selection() == 2);
    scene.update(step, kSelect);
    REQUIRE(scene.hints().page().selection() == 0);
    scene.update(step, kSelect);
    REQUIRE(scene.gatewayRequest());
    CHECK(*scene.gatewayRequest() == GatewayDestination{});
    CHECK_FALSE(scene.hints().active());
    CHECK(resident->dismissals == 1);
    CHECK(resident->answered.empty());
    // As online, the resident's own topics alone.
    talk(false);
    CHECK(labels(scene.hints().topics()) ==
          std::vector<std::string>{"General Hints", "Guardians", "exit"});
    CHECK_FALSE(scene.gatewayRequest());
    scene.close();
}
