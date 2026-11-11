#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/bosses/Bosses.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantFixture.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/level/BossKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct ChimeraFixture {
    test::FakeRenderDevice renderer;
    test::CombatantFixture fight;
    std::array<EnemyView, 1> players;
    ChimeraFixture() {
        const auto root = test::assetOrSkip("CRITTER/CHIMERA.WAD").parent_path().parent_path();
        test::assetOrSkip("MONSTERS/CHIMERA/ANIM.PS2");
        fight.open(renderer, root, nullptr, {}, 'A');
        REQUIRE(fight.spawn("CHIMERA", Vec3{0}, 0));
        players[0].player = 0;
        players[0].position = Vec3{0, 0, 40};
        players[0].height = 6;
        players[0].radius = 1;
    }
    void step() { fight.update(2, 1.0f / 30.0f, players); }
};

/** The synthetic two-headed body of HeadedBodyFixture, spawned and ready to fight. */
struct HeadedFixture {
    test::FakeRenderDevice renderer;
    test::CombatantFixture fight;
    std::array<EnemyView, 1> players;
    HeadedFixture() {
        fight.open(renderer, test::headedBodyAssets(), nullptr, {}, 'A');
        REQUIRE(fight.spawn("CHIMERA", Vec3{0}, 0));
        players[0].player = 0;
        players[0].position = Vec3{0, 0, 40};
        players[0].height = 6;
        players[0].radius = 1;
    }
    void step() { fight.update(2, 1.0f / 30.0f, players); }
};

/** One floor object flagged to move under the arena's middle, set to a height as it goes. */
struct MovingFloor {
    WorldCollision collision;
    MovingFloor() {
        CollisionTriangle triangle;
        triangle.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
        triangle.object = 0;
        collision.build({triangle});
        collision.setMovingObjects(std::array<s32, 1>{0});
        at(0);
    }
    void at(f32 height) {
        collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, height, 0}));
    }
};

/** The body and every head stand at `height`. */
void standsAt(const Combatant& body, f32 height) {
    CHECK(body.position().y == Approx(height));
    for (s32 part = 1; part <= static_cast<s32>(body.childCount()); ++part) {
        const Combatant* head = body.child(body.id() + part);
        REQUIRE(head != nullptr);
        CHECK(head->position().y == Approx(height));
    }
}

/** How high `node` of `fighter` is drawn at `alpha` between its last two updates. */
f32 drawnHeight(const Combatant& fighter, std::string_view node, f32 alpha) {
    const auto placed = fighter.presentedNodeTransform(node, alpha);
    REQUIRE(placed.has_value());
    return (*placed)[3].y;
}

/** The start, middle and end of the time between two updates. */
constexpr std::array kAlphas{0.0f, 0.5f, 1.0f};

/** The ticks and seconds of one update. */
struct UpdateStep {
    s32 ticks = 0;
    f32 seconds = 0.0f;
};

/** An update at `fps` updates a second as PlayScene deals it: two of its sixty ticks a second
 * at 30, one at 60, and at 120, its clock set to 120, one tick of 1/120 s. It never deals an
 * update without a tick. */
UpdateStep updateAt(s32 fps) {
    return {.ticks = std::max(1, Combatant::kTicksPerSecond / fps),
            .seconds = 1.0f / static_cast<f32>(fps)};
}

TEST_CASE("a boss rides a moving floor asleep and fighting, its heads with it, and keeps its "
          "height on still ground",
          "[game][chimera][boss-platform]") {
    // CritterBossAI (0x80039AD8) probes its floor every frame in every state, asleep too.
    MovingFloor floor;
    test::FakeRenderDevice renderer;
    Bosses bosses;
    bosses.open(renderer, test::headedBodyAssets(), &floor.collision, {}, 'A');
    REQUIRE(bosses.spawn(boss_type::kChimera, Vec3{0}, 0, 20));
    const Combatant& body = bosses.fighter();
    REQUIRE(body.childCount() == 2);
    constexpr f32 kStep = 1.0f / 30.0f;
    std::array<EnemyView, 1> party{};
    party[0].player = 0;
    party[0].position = Vec3{0, 0, 100}; // beyond its threshold, so it sleeps
    party[0].height = 6;
    party[0].radius = 1;
    for (s32 frame = 0; frame < 30; ++frame) {
        bosses.update(2, kStep, party);
        standsAt(body, 0);
    }
    REQUIRE_FALSE(bosses.view().awake);
    // Asleep it goes down with the floor, on an update without a tick as well, and comes up
    // again with a floor that rises within the probe's reach.
    for (s32 frame = 1; frame <= 30; ++frame) {
        const f32 height = -0.5f * static_cast<f32>(frame);
        floor.at(height);
        bosses.update(2, kStep, party);
        standsAt(body, height);
    }
    floor.at(-20);
    bosses.update(0, 0, party);
    standsAt(body, -20);
    floor.at(-17);
    bosses.update(2, kStep, party);
    standsAt(body, -17);
    REQUIRE_FALSE(bosses.view().awake);
    // Awake and fighting, it rides on, on an update without a tick as well, which only a direct
    // caller makes (PlayScene deals at least one).
    bosses.wake();
    party[0].position = Vec3{0, -17, 40};
    for (s32 frame = 1; frame <= 60; ++frame) {
        const f32 height = -17.0f - 0.25f * static_cast<f32>(frame);
        floor.at(height);
        bosses.update(2, kStep, party);
        bosses.takeCues();
        standsAt(body, height);
    }
    floor.at(-35);
    bosses.update(0, 0, party);
    standsAt(body, -35);
    REQUIRE(bosses.view().awake);
    REQUIRE(bosses.view().alive);
}

TEST_CASE("a frozen boss, and the boss as drawn between two updates, follow the floor under it",
          "[game][chimera][combatant][boss-platform]") {
    const auto root = test::headedBodyAssets();
    test::FakeRenderDevice renderer;
    // A twin on a floor that stays where it is, stepped in lockstep with it, shows what the
    // update itself does to how the body is drawn.
    MovingFloor floor;
    const MovingFloor still;
    test::CombatantFixture fight;
    test::CombatantFixture twin;
    fight.open(renderer, root, &floor.collision, {}, 'A');
    twin.open(renderer, root, &still.collision, {}, 'A');
    REQUIRE(fight.spawn("CHIMERA", Vec3{0}, 0));
    REQUIRE(twin.spawn("CHIMERA", Vec3{0}, 0));
    Combatant& body = fight.actor;
    constexpr f32 kStep = 1.0f / 30.0f;
    std::array<EnemyView, 1> players{};
    players[0].player = 0;
    players[0].position = Vec3{0, 0, 40};
    players[0].height = 6;
    players[0].radius = 1;
    // The floor goes five down in an update that runs on through the move the twin plays, so
    // that what is drawn between that update's two ends blends from the first to the second
    // instead of cutting to it.
    bool dropped = false;
    for (s32 frame = 0; frame < 120 && !dropped; ++frame) {
        const u64 generation = twin.actor.animation().generation();
        twin.update(2, kStep, players);
        twin.actor.takeCues();
        dropped = frame >= 10 && twin.actor.animation().generation() == generation;
        if (dropped) {
            floor.at(-5);
        }
        fight.update(2, kStep, players);
        body.takeCues();
    }
    REQUIRE(dropped);
    REQUIRE(body.animation().generation() == twin.actor.animation().generation());
    // By the update's end the body and its heads stand on the floor where it went; drawn, the
    // body goes down with it across the update.
    standsAt(body, -5);
    standsAt(twin.actor, 0);
    for (const f32 alpha : kAlphas) {
        CAPTURE(alpha);
        CHECK(drawnHeight(body, "", alpha) ==
              Approx(drawnHeight(twin.actor, "", alpha) - (5 * alpha)));
        CHECK(drawnHeight(body, "HEAD_L_TIP", alpha) ==
              Approx(drawnHeight(twin.actor, "HEAD_L_TIP", alpha) - (5 * alpha)));
    }
    // Frozen it stands as it is, but on the floor under it.
    body.freeze(600);
    for (s32 frame = 1; frame <= 30; ++frame) {
        const f32 height = -5.0f - 0.5f * static_cast<f32>(frame);
        floor.at(height);
        fight.update(2, kStep, players);
        REQUIRE(body.frozen());
        standsAt(body, height);
    }
}

TEST_CASE("a boss that steps up a slope stands on the floor where its step ends in the same "
          "update",
          "[game][chimera][boss-platform]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    // A body that walks straight ahead at thirty a second, within a hundred of home, with one
    // head on it.
    constexpr f32 kPace = 30;
    const auto root = test::headedBodyAssets();
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000,"radius":3,"childIndex":1,"roamRadius":100},
               {"rootNode":"HEAD_L","moveIndex":1,"moveCount":1,"maxHealth":100,"radius":2,
                "parentIndex":0,"childIndex":-1}],
      "moves":[{"name":"READY","anim":"READY","type":32,"speed":30},
               {"name":"READY","anim":"READY","type":32}]})");
    // Ground that rises a quarter of a unit with every unit ahead; its normal holds that only
    // to within f32.
    constexpr f32 kRise = 0.25f;
    constexpr f32 kHeightMargin = 1e-4f;
    CollisionTriangle ground;
    ground.vertices = {Vec3{-100, -25, -100}, Vec3{100, -25, -100}, Vec3{0, 25, 100}};
    ground.normal = glm::normalize(Vec3{0, 1, -kRise});
    ground.object = 0;
    WorldCollision slope;
    slope.build({ground});
    test::FakeRenderDevice renderer;
    test::CombatantFixture fight;
    fight.open(renderer, root, &slope, {}, 'A');
    REQUIRE(fight.spawn("CHIMERA", Vec3{0}, 0));
    const Combatant& body = fight.actor;
    REQUIRE(body.childCount() == 1);
    const std::array players{EnemyView{.player = 0, .position = Vec3{0, 0, 90}}};
    // Two thirds of a second takes it twenty up the slope, five above where it began.
    const UpdateStep step = updateAt(fps);
    f32 walked = 0;
    for (s32 frame = 0; frame < 2 * fps / 3; ++frame) {
        CAPTURE(frame);
        fight.update(step.ticks, step.seconds, players);
        walked += kPace * step.seconds;
        REQUIRE(body.position().z == Approx(walked));
        CHECK(body.position().y == Approx(kRise * walked).margin(kHeightMargin));
        CHECK(drawnHeight(body, "", 1) == Approx(kRise * walked).margin(kHeightMargin));
        // The head takes the body's place after the step whenever the body's probe comes, so
        // this only guards that it rides on the body.
        CHECK(body.child(1)->position().y == Approx(body.position().y));
    }
    CHECK(walked == Approx(20));
}

TEST_CASE("a boss that dies on a moving floor is defeated where the update before its removal "
          "left it",
          "[game][bosses][boss-platform]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    MovingFloor floor;
    test::FakeRenderDevice renderer;
    Bosses bosses;
    bosses.open(renderer, test::headedBodyAssets(), &floor.collision, {}, 'A');
    REQUIRE(bosses.spawn(boss_type::kChimera, Vec3{0}, 0, 20));
    const std::array party{EnemyView{.player = 0, .position = Vec3{0, 0, 500}}};
    bosses.wake();
    EnemyHit lethal;
    lethal.damage = 100000;
    bosses.hurt(lethal);
    REQUIRE(bosses.fighter().dying());
    // The floor goes half a unit down every update while it dies. Bosses::update takes the
    // defeat where the boss stood as the update that removes it began, since the original
    // removes it before its floor probe, so it falls on the floor of the update before.
    const UpdateStep step = updateAt(fps);
    std::optional<Vec3> defeat;
    for (s32 update = 1; update <= 4 * fps && !defeat; ++update) {
        CAPTURE(update);
        const f32 height = -0.5f * static_cast<f32>(update);
        floor.at(height);
        bosses.update(step.ticks, step.seconds, party);
        bosses.takeCues();
        bosses.takeSpews();
        defeat = bosses.takeDefeat();
        if (defeat) {
            CHECK(defeat->y == Approx(height + 0.5f));
            CHECK_FALSE(bosses.present());
        } else {
            standsAt(bosses.fighter(), height);
        }
    }
    REQUIRE(defeat.has_value());
}

TEST_CASE("asleep, a boss measures the party from where its previous update left it, then keeps "
          "to its floor",
          "[game][bosses][boss-platform]") {
    MovingFloor floor;
    test::FakeRenderDevice renderer;
    Bosses bosses;
    bosses.open(renderer, test::headedBodyAssets(), &floor.collision, {}, 'A');
    REQUIRE(bosses.spawn(boss_type::kChimera, Vec3{0}, 0, 20));
    const Combatant& body = bosses.fighter();
    constexpr f32 kStep = 1.0f / 30.0f;
    std::array<EnemyView, 1> party{};
    party[0].player = 0;
    party[0].position = Vec3{0, 0, 30}; // beyond its twenty, so it sleeps
    // Its two-second wait is over before the measured update: seventy-two updates of 1/30 s
    // come to 2.4 s in f32 (sixty would come to 1.9999994 s), whichever side of the wake test
    // the clock steps on. Only the party's distance decides the measured update, taken in
    // three dimensions; the party is placed for it before the boss's update, as the scene runs
    // them.
    constexpr s32 kAsleep = 72;
    for (s32 frame = 0; frame < kAsleep; ++frame) {
        bosses.update(2, kStep, party);
        standsAt(body, 0);
    }
    REQUIRE_FALSE(bosses.view().awake);
    SECTION("a party within reach of where it stood wakes it, though its floor has gone down") {
        // 19.9 from where it stood; 20.5 from the floor it goes down to.
        floor.at(-5);
        party[0].position = Vec3{0, 0, 19.9f};
        bosses.update(2, kStep, party);
        CHECK(bosses.view().awake);
        standsAt(body, -5);
    }
    SECTION("a party within reach only of where its floor takes it lets it sleep on") {
        // 20.1 from where it stood; 19.9 from the floor it goes down to.
        floor.at(-3);
        party[0].position = Vec3{0, -3, 19.9f};
        bosses.update(2, kStep, party);
        CHECK_FALSE(bosses.view().awake);
        standsAt(body, -3);
        bosses.update(2, kStep, party);
        CHECK(bosses.view().awake);
        standsAt(body, -3);
    }
}

TEST_CASE("asleep, an entrance held in its INIT is drawn going down with its floor across an "
          "update and stands on it by the update's end",
          "[game][bosses][boss-platform]") {
    // The headed body with an INIT it holds until it wakes, as a decoded encounter does.
    const auto root = test::headedBodyAssets();
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":3,"maxHealth":1000,"radius":3,"childIndex":1},
               {"rootNode":"HEAD_L","moveIndex":3,"moveCount":1,"maxHealth":100,"radius":2,
                "parentIndex":0,"childIndex":-1}],
      "moves":[{"name":"INIT","anim":"READY","type":0},
               {"name":"START","anim":"START","type":16,"priority":4095,"interrupt":0},
               {"name":"READY","anim":"READY","type":32},
               {"name":"READY","anim":"READY","type":32}]})");
    test::FakeRenderDevice renderer;
    auto stock = std::make_unique<CombatantAssets>();
    REQUIRE(stock->load(renderer, root, bossDefinition("CHIMERA"), 'A'));
    stock->initialMoveBeforeWake = true;
    MovingFloor floor;
    Bosses bosses;
    bosses.open(renderer, root, &floor.collision, {}, 'A');
    REQUIRE(bosses.preload(boss_type::kChimera, std::move(stock)) != nullptr);
    REQUIRE(bosses.spawn(boss_type::kChimera, Vec3{0}, 0, 20));
    const Combatant& body = bosses.fighter();
    REQUIRE(body.childCount() == 1);
    constexpr f32 kStep = 1.0f / 30.0f;
    const std::array party{EnemyView{.player = 0, .position = Vec3{0, 0, 500}}};
    for (s32 frame = 0; frame < 5; ++frame) {
        bosses.update(2, kStep, party);
    }
    // What is drawn was taken as its INIT went on, before its probe stood it on the floor.
    floor.at(-2);
    bosses.update(2, kStep, party);
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(body.moveType() == MoveDefinition::kInit);
    standsAt(body, -2);
    for (const f32 alpha : kAlphas) {
        CAPTURE(alpha);
        CHECK(drawnHeight(body, "", alpha) == Approx(-2 * alpha));
    }
}

TEST_CASE("the body's pattern drives the heads' own pattern step, whose move starts over each "
          "time it ends while the body's step lasts",
          "[game][chimera][combatant]") {
    HeadedFixture fixture;
    Combatant& body = fixture.fight.actor;
    // Past its entrance the body, with the player thirty or more away, runs its pattern: two
    // stances, each a second, while each head spits on its own step of its pattern.
    s32 stepStarts = 0;
    s32 spits = 0;
    std::string lastStep;
    for (s32 frame = 0; frame < 45; ++frame) {
        fixture.step();
        for (const auto& cue : body.takeCues()) {
            spits += cue.critter == 1 && cue.tree == "SPITFX" ? 1 : 0;
        }
        if (body.moveName() == "READYP" && lastStep != "READYP") {
            ++stepStarts;
        }
        lastStep = std::string{body.moveName()};
        if (stepStarts > 0 && body.moveName() != "READYP") {
            break;
        }
    }
    REQUIRE(stepStarts == 1);
    REQUIRE(body.moveName() == "READYP");
    REQUIRE(body.child(1)->moveName() == "SPIT");
    // A six-frame spit within a thirty-frame step: started over four times at the least.
    REQUIRE(spits >= 4);
    // Its own use is not recorded (CritterMoveDone leaves a pattern's child alone). With
    // one untouched player, distribution keeps the root and the last tied head: head 2
    // spits again by its own choice once the body's pattern is over.
    bool ownSpit = false;
    for (s32 frame = 0; frame < 120 && !ownSpit; ++frame) {
        fixture.step();
        body.takeCues();
        ownSpit = body.moveName() != "READYP" && body.child(2)->moveName() == "SPIT";
    }
    REQUIRE(ownSpit);
    REQUIRE(body.moveType() == 1); // and the body keeps to SYNC while a head attacks
}

TEST_CASE("the body falls with its last head, after each head's own fall",
          "[game][chimera][combatant]") {
    HeadedFixture fixture;
    Combatant& body = fixture.fight.actor;
    for (s32 frame = 0; frame < 10; ++frame) {
        fixture.step();
        body.takeCues();
    }
    body.takeLosses();
    EnemyHit hit;
    hit.damage = 1000;
    const f32 bodyHealth = body.health();
    const f32 headHealth = body.child(1)->health();
    CHECK(body.hurt(hit, 1) == Approx(headHealth));
    CHECK(body.health() == bodyHealth); // a lethal head hit still earns its healing credit
    REQUIRE_FALSE(body.child(1)->alive());
    REQUIRE(body.alive());
    fixture.step();
    REQUIRE(body.alive()); // one head standing keeps it up
    body.hurt(hit, 2);
    REQUIRE_FALSE(body.child(2)->alive());
    REQUIRE(body.alive()); // the lethal hit on a head is not passed on; it falls next frame
    fixture.step();
    REQUIRE(body.dying());
    REQUIRE(body.health() < 0);
    const auto losses = body.takeLosses();
    REQUIRE(std::ranges::count_if(losses, [](const auto& loss) { return loss.killed; }) == 3);
    REQUIRE(std::ranges::count_if(
                losses, [](const auto& loss) { return loss.killed && loss.critter == 0; }) == 1);
}

TEST_CASE("head hit flashes expire independently of body-controlled animation",
          "[game][chimera][combatant][hit-feedback]") {
    HeadedFixture fixture;
    auto& body = fixture.fight.actor;
    SECTION("during the shared entrance") {
        REQUIRE(body.moveType() == MoveDefinition::kStart);
    }
    SECTION("while the body holds the heads") {
        body.hold(true);
    }
    SECTION("while the body is frozen") {
        body.freeze(120);
    }
    SECTION("during independent head attacks") {
        for (s32 frame = 0; frame < 120 && body.moveType() != 1; ++frame) {
            fixture.step();
        }
        REQUIRE(body.moveType() == 1);
    }
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = 0x100000;
    body.hurt(hit, 1);
    REQUIRE(body.child(1)->flashing());
    test::FakeTexture flash{1, 1};
    const auto drawsFlash = [&] {
        fixture.renderer.draws.clear();
        body.draw(fixture.renderer, Mat4{1}, {}, nullptr, nullptr, &flash);
        REQUIRE_FALSE(fixture.renderer.draws.empty());
        return std::ranges::any_of(fixture.renderer.draws, [&](const auto& draw) {
            return draw.state.maskedTexture == &flash;
        });
    };
    REQUIRE(drawsFlash());
    fixture.step();
    CHECK(body.child(1)->flashing());
    CHECK(drawsFlash());
    fixture.step();
    CHECK_FALSE(body.child(1)->flashing());
    CHECK_FALSE(drawsFlash());
}

TEST_CASE("native Chimera head skin clears after a hit during its shared entrance",
          "[game][chimera][assets][hit-feedback]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    REQUIRE(body.moveType() == MoveDefinition::kStart);
    EnemyHit hit;
    hit.damage = 3;
    hit.flags = 0x100000;
    body.hurt(hit, 2);
    REQUIRE(body.child(2)->flashing());
    test::FakeTexture flash{1, 1};
    body.draw(fixture.renderer, Mat4{1}, {}, nullptr, nullptr, &flash);
    REQUIRE(std::ranges::any_of(fixture.renderer.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == &flash;
    }));
    fixture.step();
    CHECK(body.child(2)->flashing());
    fixture.step();
    CHECK_FALSE(body.child(2)->flashing());
    fixture.renderer.draws.clear();
    body.draw(fixture.renderer, Mat4{1}, {}, nullptr, nullptr, &flash);
    REQUIRE_FALSE(fixture.renderer.draws.empty());
    CHECK(std::ranges::none_of(fixture.renderer.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == &flash;
    }));
}

TEST_CASE("Chimera loads and runs all three head move tables", "[game][chimera][assets]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    REQUIRE(body.childCount() == 3);
    REQUIRE(body.child(1)->maxHealth() == 1200);
    REQUIRE(body.child(2)->maxHealth() == 1500);
    REQUIRE(body.child(3)->maxHealth() == 1200);
    // These come from the loaded native CRITTER/CHIMERA.WAD, not unpacked JSON.
    CHECK(body.child(1)->data()->sight().yaw == Approx(-std::numbers::pi_v<f32> / 6));
    CHECK(body.child(2)->data()->sight().yaw == 0);
    CHECK(body.child(3)->data()->sight().yaw == Approx(std::numbers::pi_v<f32> / 6));
    REQUIRE(body.data()->patterns().size() == 1);
    const auto& pattern = body.data()->patterns().front();
    CHECK(pattern.cooldown == 16);
    CHECK(pattern.target.minDistance == 35);
    CHECK(pattern.target.minDot == Approx(0).margin(0.000001));
    CHECK(pattern.target.minRateScale == 0);
    CHECK(pattern.target.maxRateScale == 0);
    for (s32 id = 1; id <= 3; ++id) {
        REQUIRE(body.child(id)->data()->moves().size() == 15);
        REQUIRE(body.child(id)->moveType() == -1); // START is copied from the body.
    }
    std::set<s32> shooters;
    std::set<std::string> effects;
    bool sync = false;
    for (s32 frame = 0; frame < 3600; ++frame) {
        // Fight back often enough to retain the body's fifteen-second anger memory.
        // An untouched player straight ahead only engages the lion independently:
        // the side heads' authored sight yaw gives them weaker claims under cap two.
        if (frame % 300 == 0) {
            EnemyHit hit;
            hit.player = fixture.players[0].player;
            hit.damage = 12; // Ten after armor raises candidate capacity to all four parts.
            body.hurt(hit);
        }
        // Move the target between projectile and close-combat ranges, not an idle launch.
        fixture.players[0].position = Vec3{0, 0, frame < 1800 ? 40.0f : 18.0f};
        fixture.step();
        sync |= body.moveType() == 1;
        for (const auto& shot : body.takeShots()) {
            if (shot.critter > 0) {
                shooters.insert(shot.critter);
                REQUIRE(shot.data == body.child(shot.critter)->data());
                REQUIRE(shot.target.has_value());
                REQUIRE(glm::distance(shot.origin, body.position()) > 1);
            }
        }
        for (const auto& cue : body.takeCues()) {
            if (cue.critter > 0 && !cue.tree.empty()) {
                effects.insert(cue.tree);
            }
        }
        body.takeBlows();
    }
    REQUIRE(shooters == std::set<s32>{1, 2, 3});
    REQUIRE(sync);
    REQUIRE_FALSE(effects.empty());
}

TEST_CASE("Chimera routes head damage and shares body damage without applying armor twice",
          "[game][chimera][assets]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    EnemyHit hit;
    hit.damage = 62; // TYPE armor is two; sixty reaches the selected creature.
    body.hurt(hit);
    REQUIRE(body.health() == Approx(5940));
    REQUIRE(body.child(1)->health() == Approx(1190));
    REQUIRE(body.child(2)->health() == Approx(1490));
    REQUIRE(body.child(3)->health() == Approx(1190));
    body.hurt(hit, 2);
    REQUIRE(body.health() == Approx(5880));
    REQUIRE(body.child(2)->health() == Approx(1430));
    REQUIRE(body.child(1)->health() == Approx(1190));
    hit.damage = 2000;
    body.draw(fixture.renderer, Mat4{1}, {});
    const usize beforeDraws = fixture.renderer.draws.size();
    body.hurt(hit, 2);
    REQUIRE_FALSE(body.child(2)->alive());
    REQUIRE(body.alive());
    REQUIRE(body.health() == Approx(5880)); // Lethal child hits do not forward.
    const auto targets = body.bodyTargets();
    REQUIRE(std::ranges::none_of(targets, [](const auto& t) { return t.id == 2; }));
    REQUIRE(std::ranges::any_of(targets, [](const auto& t) { return t.id == 1; }));
    bool stump = false;
    for (s32 frame = 0; frame < 180; ++frame) {
        fixture.step();
        for (const auto& cue : body.takeCues()) {
            if (cue.tree == "STUMPL") {
                REQUIRE_FALSE(stump);
                stump = true;
                REQUIRE(cue.critter == 2);
                REQUIRE(cue.node == "BODY1_LION");
                REQUIRE(cue.placement.has_value());
            }
        }
        for (const auto& shot : body.takeShots()) {
            REQUIRE(shot.critter != 2);
        }
    }
    REQUIRE(stump);
    REQUIRE(body.child(2)->dying());
    fixture.renderer.draws.clear();
    body.draw(fixture.renderer, Mat4{1}, {});
    REQUIRE_FALSE(fixture.renderer.draws.empty());
    REQUIRE(fixture.renderer.draws.size() < beforeDraws);
}

TEST_CASE("Chimera scimitar removes the lion only on impact, without subtracting body health",
          "[game][chimera][assets]") {
    const auto root = test::assetOrSkip("CRITTER/CHIMERA.WAD").parent_path().parent_path();
    test::FakeRenderDevice renderer;
    Bosses bosses;
    bosses.open(renderer, root, nullptr, {}, 'A');
    REQUIRE(bosses.spawn(35, Vec3{0}, 0));
    REQUIRE(bosses.bringLegend(0));
    const f32 health = bosses.view().health;
    EnemyView player;
    player.player = 0;
    player.position = Vec3{0, 0, 40};
    const std::array players{player};
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(2, 1.0f / 30.0f, players);
        bosses.takeCues();
    }
    REQUIRE(bosses.legend().thrown());
    const auto before = bosses.targets();
    REQUIRE(std::ranges::any_of(before, [](const auto& target) { return target.id == 2; }));
    REQUIRE(bosses.view().health == health);
    bosses.landLegend();
    const auto after = bosses.targets();
    REQUIRE(std::ranges::none_of(after, [](const auto& target) { return target.id == 2; }));
    REQUIRE(std::ranges::any_of(after, [](const auto& target) { return target.id == 1; }));
    REQUIRE(std::ranges::any_of(after, [](const auto& target) { return target.id == 3; }));
    REQUIRE(bosses.view().health == health);
    const auto losses = bosses.takeLosses();
    REQUIRE(std::ranges::count_if(losses, [](const auto& loss) { return loss.killed; }) == 1);
    bosses.landLegend();
    REQUIRE(bosses.takeLosses().empty());
    bool stump = false;
    for (s32 frame = 0; frame < 90; ++frame) {
        bosses.update(2, 1.0f / 30.0f, players);
        for (const auto& cue : bosses.takeCues()) {
            REQUIRE(cue.tree != "STUMPL");
            stump |= cue.tree == "STUMPLQ";
        }
    }
    REQUIRE(stump);
}

TEST_CASE("Chimera's heads turn to the party from their necks", "[game][chimera][assets]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    REQUIRE(body.child(1)->data()->looks()[0].node == "BODY1_ESTEVE");
    REQUIRE(body.child(2)->data()->looks()[0].node == "BODY1_LSTEVE");
    REQUIRE(body.child(3)->data()->looks()[0].node == "BODY1_SHEAD");
    REQUIRE(body.child(1)->data()->looks()[0].yawRate == Approx(0.785f).margin(1e-3f));
    REQUIRE(body.data()->looks()[0].node.empty());
    // Held to its stance the body animates the same whoever stands where; the heads' necks
    // differ only by where they turn to look. The eagle's neck comes out of the stance turned
    // well to the left of a player straight ahead, so ahead it is at its limit and only a
    // player round to the left is within it: the two settle apart.
    const auto neckYaw = [](ChimeraFixture& given, const Vec3& player, const char* node) {
        // MoveSetup must first assign a real head target. CopyParentAnimation keeps
        // that identity for gaze, but holding the body from spawn never assigns one.
        EnemyHit hit;
        hit.player = 0;
        hit.damage = 12;
        given.fight.actor.hurt(hit); // permits all heads in the target roster
        for (s32 frame = 0; frame < 600 && given.fight.actor.child(1)->moveName().empty();
             ++frame) {
            given.step();
            given.fight.actor.takeCues();
        }
        REQUIRE_FALSE(given.fight.actor.child(1)->moveName().empty());
        given.players[0].position = player;
        given.fight.actor.hold(true);
        for (s32 frame = 0; frame < 120; ++frame) {
            given.step();
            given.fight.actor.takeCues();
        }
        const Mat4 world = *given.fight.actor.nodeTransform(node);
        return std::atan2(world[2].x, world[2].z);
    };
    ChimeraFixture other;
    const f32 ahead = neckYaw(fixture, Vec3{0, 0, 40}, "BODY1_ESTEVE");
    const f32 beside = neckYaw(other, Vec3{-40, 0, 10}, "BODY1_ESTEVE");
    CAPTURE(ahead, beside);
    REQUIRE(beside < ahead - 0.3f);
}

TEST_CASE("Chimera shared death stops surviving heads and emits only body reward",
          "[game][chimera][assets]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    for (s32 frame = 0; frame < 300; ++frame) {
        fixture.step();
        body.takeShots();
        body.takeCues();
        body.takeBlows();
    }
    EnemyHit hit;
    hit.damage = 10000;
    body.hurt(hit);
    REQUIRE(body.dying());
    s32 rewards = 0;
    for (s32 frame = 0; frame < 900 && body.present(); ++frame) {
        fixture.step();
        REQUIRE(body.takeShots().empty());
        REQUIRE(body.takeBlows().empty());
        for (const auto& spew : body.takeSpews()) {
            REQUIRE(spew.critter == 0);
            ++rewards;
        }
        body.takeCues();
    }
    REQUIRE_FALSE(body.present());
    REQUIRE(rewards == 1);
}
} // namespace
