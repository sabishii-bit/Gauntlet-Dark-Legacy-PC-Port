#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

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
#include "game/party/PartyTeleport.h"
#include "game/players/PlayerControls.h"
#include "game/scenes/GatewayTower.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/TransitionScreen.h"
#include "game/tower/GatewayDeparture.h"
#include "game/tower/HintMenu.h"
#include "game/tower/SumnerFigure.h"
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

/** How many of the teleport's sparkles are playing in the scene. */
usize sparkles(const PlayScene& scene) {
    usize count = 0;
    for (usize i = 0; i < scene.effects().count(); ++i) {
        count += scene.effects().effect(i).name == PartyTeleport::kEffect ? 1 : 0;
    }
    return count;
}

/** The ticks of 60 a second that an update of `step` seconds runs. */
s32 ticksOf(f64 step) {
    return static_cast<s32>(std::lround(step * 60.0));
}

/** When each moment of a departure through the gateways came, in ticks from the choice. */
struct DepartureTimes {
    std::optional<s32> scrollGone; ///< Sumner's scroll gone
    std::optional<s32> sparkled;   ///< the teleport on the party
    std::optional<s32> left;       ///< the party gone, the transition coming up
    std::optional<s32> travelled;  ///< the transition covering: the scene says Travel
    usize sparkles = 0;            ///< the most teleport sparkles playing at once
};

/** Runs a scene whose party has just chosen a place past Sumner's gateways until it says
 * Travel, the first seat trying to walk all the while: the party stands held where it chose,
 * and nobody may pause or join. */
DepartureTimes followDeparture(PlayScene& scene, f64 step) {
    PlayScene::Inputs walk{};
    walk[0].move = MoveInput{Vec2{0.0f, 1.0f}, 1.0f};
    REQUIRE(scene.actor(0) != nullptr);
    const Vec3 stood = scene.actor(0)->position();
    const usize before = sparkles(scene);
    DepartureTimes times;
    s32 elapsed = 0;
    for (s32 update = 0; update < 1200 && !times.travelled; ++update) {
        CHECK_FALSE(scene.canPause(0));
        CHECK_FALSE(scene.canJoin(1));
        const PlayOutcome outcome = scene.update(step, walk);
        elapsed += ticksOf(step);
        CHECK(scene.actor(0)->position() == stood);
        if (!times.scrollGone && !scene.hints().active()) {
            times.scrollGone = elapsed;
        }
        if (!times.sparkled && sparkles(scene) > before) {
            times.sparkled = elapsed;
        }
        times.sparkles = std::max(times.sparkles, sparkles(scene) - before);
        if (!times.left && scene.leaving()) {
            times.left = elapsed;
        }
        if (outcome == PlayOutcome::Travel) {
            times.travelled = elapsed;
        } else {
            CHECK(outcome == PlayOutcome::Running);
            // Where the party went is known from the moment it leaves.
            CHECK(scene.gatewayDeparture().has_value() == scene.leaving());
        }
    }
    return times;
}

/** The departure keeps the good wizard's time at either rate: the teleport as soon as Sumner's
 * scroll has gone, the party gone 35 ticks on (DoGoodWizard 0x80019090, 0x80343B60), and the
 * transition's two seconds before the journey. */
void checkDepartureTimes(const DepartureTimes& times, f64 step) {
    const s32 ticks = ticksOf(step);
    REQUIRE(times.scrollGone);
    REQUIRE(times.sparkled);
    REQUIRE(times.left);
    REQUIRE(times.travelled);
    CHECK(*times.sparkled >= *times.scrollGone);
    CHECK(*times.sparkled - *times.scrollGone <= ticks);
    CHECK(*times.left - *times.sparkled >= GatewayDeparture::kSparkleTicks);
    CHECK(*times.left - *times.sparkled < GatewayDeparture::kSparkleTicks + ticks);
    const auto covering = static_cast<s32>(TransitionScreen::kFadeInSeconds * 60.0f);
    CHECK(*times.travelled - *times.left >= covering);
    CHECK(*times.travelled - *times.left <= covering + 2 * ticks);
}

/** A journey called off leaves the party where it stood, in play again: it may pause, join and
 * walk, and the transition clears away. */
void checkCalledOff(PlayScene& scene, f64 step, bool tower) {
    REQUIRE(scene.actor(0) != nullptr);
    const Vec3 stood = scene.actor(0)->position();
    REQUIRE(scene.cancelGatewayDeparture());
    CHECK_FALSE(scene.leaving());
    CHECK_FALSE(scene.gatewayRequest());
    CHECK_FALSE(scene.gatewayDeparture());
    CHECK(scene.transition().phase() == TransitionScreen::Phase::Clearing);
    CHECK(scene.canPause(0));
    CHECK(scene.canJoin(1) == tower);
    CHECK_FALSE(scene.cancelGatewayDeparture()); // none under way now
    PlayScene::Inputs walk{};
    walk[0].move = MoveInput{Vec2{0.0f, 1.0f}, 1.0f};
    for (s32 i = 0; i < 45; ++i) {
        CHECK(scene.update(step, walk) == PlayOutcome::Running);
    }
    CHECK(scene.actor(0)->position() != stood);
    CHECK_FALSE(scene.transition().showing());
}

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

TEST_CASE("a place chosen past Dark Legacy's gateways sees the party teleported away to it",
          "[game][screens][gateways][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    // The first world past the gateways, the N64's tower, or the second, the Dreamcast's.
    const usize row = GENERATE(1, 2);
    CAPTURE(step, row);
    GatewayTower tower;
    tower.gateways.offered[1].usable = true;
    PlayScene& scene = tower.scene;
    visitSumner(tower, std::vector{PartyMember{0, toldOfGateways()}}, step);
    for (s32 i = 0; i < 4; ++i) {
        scene.update(step, kDown);
    }
    scene.update(step, kSelect);
    REQUIRE(scene.hints().reading());
    while (scene.hints().page().selection() < static_cast<s32>(row)) {
        scene.update(step, kDown);
    }
    REQUIRE(scene.hints().page().selection() == static_cast<s32>(row));
    scene.update(step, kSelect);
    const GatewayDestination chosen{static_cast<u8>(row - 1)};
    REQUIRE(scene.gatewayRequest() == chosen);
    // His scroll burns away under his sweep, where a visit done gets his wave.
    CHECK(scene.hints().burning());
    CHECK(scene.sumner().playing(SumnerFigure::kGestureIndex));
    CHECK_FALSE(scene.sumner().playing(SumnerFigure::kGoAwayIndex));
    // Held still, the party sparkles once the scroll has gone and leaves 35 ticks on; when the
    // transition covers the view, the journey is the application's.
    const DepartureTimes times = followDeparture(scene, step);
    checkDepartureTimes(times, step);
    CHECK(times.sparkles == 1); // one at the one member's feet
    CHECK(scene.gatewayDeparture() == chosen);
    CHECK(scene.gatewayRequest() == chosen);
    CHECK(scene.leaving());
    CHECK(scene.transition().covering());
    // A world that does not open leaves the party where it stood, in play again.
    checkCalledOff(scene, step, true);
    scene.close();
}

TEST_CASE("with neither Legends campaign installed, Sumner's gateways take nobody anywhere",
          "[game][screens][gateways][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    GatewayTower tower;
    tower.gateways.offered[0].usable = false; // a GameCube-only installation: neither usable
    PlayScene& scene = tower.scene;
    visitSumner(tower, std::vector{PartyMember{0, toldOfGateways()}}, step);
    for (s32 i = 0; i < 4; ++i) {
        scene.update(step, kDown);
    }
    scene.update(step, kSelect);
    REQUIRE(scene.hints().reading());
    REQUIRE(choosable(scene.hints().page()) == std::vector<bool>{false, false, false});
    // No place answers: the list stays, nobody is sent, Sumner keeps his sweep.
    for (s32 row = 0; row < 3; ++row) {
        scene.update(step, kSelect);
        CHECK(scene.hints().reading());
        CHECK_FALSE(scene.gatewayRequest());
        CHECK_FALSE(scene.sumner().playing(SumnerFigure::kGestureIndex));
        scene.update(step, kDown);
    }
    // Backed out of, the scroll burns away as ever and play goes on with nobody leaving.
    scene.update(step, kBack);
    REQUIRE_FALSE(scene.hints().reading());
    scene.update(step, kBack);
    CHECK(scene.sumner().playing(SumnerFigure::kGoAwayIndex));
    for (s32 i = 0; i < 300 && scene.hints().active(); ++i) {
        CHECK(scene.update(step, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.hints().active());
    for (s32 i = 0; i < 90; ++i) {
        CHECK(scene.update(step, {}) == PlayOutcome::Running);
    }
    CHECK_FALSE(scene.leaving());
    CHECK_FALSE(scene.gatewayDeparture());
    CHECK_FALSE(scene.cancelGatewayDeparture());
    CHECK(scene.canPause(0));
    scene.close();
}

namespace {

/** A tower's resident past the gateways, as an expansion hosts one: the topics it is asked
 * for, with the gateway row where it says, what the scene hands back to it, and how often it
 * is given its party to play or to hold still. */
class GatewayResident final : public ImportedLevel {
public:
    std::optional<ImportedDialogue> offered;
    std::vector<s32> answered;
    s32 dismissals = 0;
    s32 played = 0;
    s32 held = 0;

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
    void updateParty(std::span<PlayerActor*> /*players*/, f32 /*seconds*/,
                     bool /*timeStopped*/) override {
        ++played;
    }
    void holdParty(std::span<PlayerActor* const> /*party*/) override { ++held; }
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

/** The second world's tower hosted in the scene on a plain floor, with both worlds past the
 * gateways usable. Skips without the game's class data. */
struct ResidentTower {
    const std::filesystem::path root =
        test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    const AssetLocator assets{root};
    const GameConfig config;
    StringTable strings;
    test::FakeRenderDevice device;
    LevelWorld world;
    std::shared_ptr<GatewayResident> resident = std::make_shared<GatewayResident>();
    GameServices context;
    test::FixedGateways gateways;
    PlayScene scene;

    ResidentTower() {
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        LevelRef ref;
        ref.realm = "Legends";
        ref.name = "test:tower";
        WorldCollision collision;
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        auto layout =
            WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Start, .position = {0, 0, 0}}},
                                     {-100, 0, -100}, {100, 40, 100});
        const bool loaded = world.loadImported(device, root, ref, std::move(layout),
                                               std::move(collision), resident);
        REQUIRE(loaded);
        context.config = &config;
        context.strings = &strings;
        context.assets = &assets;
        context.unpackedRoot = root;
        gateways.offered = {{0, "legends.n64.gateway.label", true},
                            {1, "legends.dreamcast.gateway.label", true}};
    }

    /** Opens the scene offline or as online, with the party in play, until its resident's
     * menu is up, `step` an update. */
    void talk(f64 step, bool offline) {
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0};
        const std::array party{PartyMember{0, CharacterSave{}}};
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
    }

    /** Opens the gateway row's list and chooses its place at `row`. */
    void choose(f64 step, s32 row) {
        scene.update(step, kDown);
        scene.update(step, kDown);
        REQUIRE(scene.hints().topics().selection() == 2);
        scene.update(step, kSelect);
        REQUIRE(scene.hints().reading());
        REQUIRE(scene.hints().page().selection() == 0);
        for (s32 i = 0; i < row; ++i) {
            scene.update(step, kDown);
        }
        REQUIRE(scene.hints().page().selection() == row);
        scene.update(step, kSelect);
    }
};

} // namespace

TEST_CASE("a tower past the gateways lists them where its resident says, greying its own world",
          "[game][screens][gateways][imported-play][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    ResidentTower tower;
    PlayScene& scene = tower.scene;
    tower.talk(step, true);
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
    CHECK(tower.resident->answered.empty()); // the row is the scene's, never the resident's
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
    CHECK(tower.resident->dismissals == 1);
    CHECK(tower.resident->answered.empty());
    // As online, the resident's own topics alone.
    tower.talk(step, false);
    CHECK(labels(scene.hints().topics()) ==
          std::vector<std::string>{"General Hints", "Guardians", "exit"});
    CHECK_FALSE(scene.gatewayRequest());
    scene.close();
}

TEST_CASE("from a tower past the gateways the party is teleported home, or on to another world",
          "[game][screens][gateways][imported-play][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    // The way home, first on the list, or the first world past the gateways.
    const s32 row = GENERATE(0, 1);
    CAPTURE(step, row);
    ResidentTower tower;
    PlayScene& scene = tower.scene;
    tower.talk(step, true);
    tower.choose(step, row);
    const GatewayDestination chosen = row == 0 ? GatewayDestination{} : GatewayDestination{0};
    REQUIRE(scene.gatewayRequest() == chosen);
    CHECK_FALSE(scene.hints().active()); // a tower's own menu goes at once
    // The resident holds the party still through the departure, playing it no more, and its
    // own way out never begins: the transition is the scene's.
    const s32 played = tower.resident->played;
    const s32 held = tower.resident->held;
    const DepartureTimes times = followDeparture(scene, step);
    checkDepartureTimes(times, step);
    CHECK(times.sparkles == 1);
    CHECK(tower.resident->played == played);
    CHECK(tower.resident->held > held);
    CHECK_FALSE(tower.resident->departure());
    CHECK(scene.gatewayDeparture() == chosen);
    CHECK(scene.leaving());
    CHECK(scene.transition().covering());
    // A place that does not open leaves the party where it stood, in play again.
    checkCalledOff(scene, step, false);
    CHECK(tower.resident->played > played);
    scene.close();
}
