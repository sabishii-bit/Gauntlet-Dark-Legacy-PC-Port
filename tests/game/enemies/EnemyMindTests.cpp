#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/enemies/EnemyMind.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

/** A sense of standing at the origin facing +z with a player straight ahead, all clear. */
MindSense senseAhead(f32 distance = 20.0f) {
    MindSense sense;
    sense.yaw = 0.0f;
    sense.ticks = 2;
    sense.target = 0;
    sense.targetPosition = Vec3{0.0f, 0.0f, distance};
    sense.targetDistance = distance;
    sense.closeDistance = distance;
    sense.recognized = true;
    return sense;
}

TEST_CASE("the minds are found by the original's way numbers, strangers wandering",
          "[game][enemies][mind]") {
    REQUIRE(enemyMindOf(kSeekWay).name() == "seek");
    REQUIRE(enemyMindOf(kProwlWay).name() == "prowl");
    REQUIRE(enemyMindOf(kWanderWay).name() == "wander");
    REQUIRE(enemyMindOf(kWanderOtherWay).name() == "wander");
    REQUIRE(enemyMindOf(kChaseWay).name() == "chase");
    REQUIRE(enemyMindOf(kLoiterWay).name() == "loiter");
    REQUIRE(enemyMindOf(kFleeWay).name() == "flee");
    REQUIRE(enemyMindOf(kLurkWay).name() == "lurk");
    REQUIRE(enemyMindOf(kLungeWay).name() == "lunge");
    REQUIRE(enemyMindOf(19).name() == "wander");
    REQUIRE(enemyMindOf(-1).name() == "wander");
    REQUIRE(wrapAngle(kPi + 0.5f) == Approx(-kPi + 0.5f));
    REQUIRE(wrapAngle(-kPi - 0.5f) == Approx(kPi - 0.5f));
    REQUIRE(wrapAngle(1.0f) == 1.0f);
}

TEST_CASE("delegated enemy strategies reset their own state only when the effective way changes",
          "[game][enemies][mind][mind-transition]") {
    // format_brain 0x80050394 runs after move_logic's delegation gates. do_enemies retains
    // the effective algorithm in prev_ai before restoring the authored algorithm each tick.
    const auto& prowl = enemyMindOf(kProwlWay);
    MindMemory memory;
    prowl.think(memory, senseAhead(20));
    memory.heading = 0.4f;
    memory.headingBefore = memory.heading;
    memory.deadEnd = 24;
    memory.route = -2;
    memory.collided = 6;
    memory.counter = 3;

    const auto close = prowl.think(memory, senseAhead(4));
    CHECK(close.heading == Approx(0).margin(0.000001));
    CHECK(memory.deadEnd == 0);
    CHECK(memory.route == 1);
    CHECK(memory.collided == 0);

    memory.deadEnd = 12;
    prowl.think(memory, senseAhead(4));
    CHECK(memory.deadEnd == 10); // the same delegated seek does not reformat every tick

    prowl.think(memory, senseAhead(20));
    CHECK(memory.deadEnd == 0);
    CHECK(memory.counter == 0); // a resumed prowl starts a fresh four-turn count
}

TEST_CASE("enemy births initialize the authored strategy before any temporary fallback",
          "[game][enemies][mind][mind-transition]") {
    // init_enemy_vars calls format_brain before the first movement update.
    for (const s32 way : {kThrowWay, kBombWay, kSkirmishWay, kSkirmishBombWay}) {
        MindMemory memory;
        initializeEnemyMind(memory, way, 19);
        CHECK(memory.effectiveWay == way);
        CHECK(memory.fuse == 9);
        auto sense = senseAhead(24);
        const auto& mind = enemyMindOf(way);
        CHECK_FALSE(mind.think(memory, sense).throwing);
        CHECK(memory.fuse == 7);
        mind.think(memory, sense);
        CHECK(memory.fuse == 5);
    }
    for (const s32 way : {kStandCastWay, kRangeCastWay, kCastWay, kLungeWay}) {
        MindMemory memory;
        memory.heading = 0.4f;
        memory.headingBefore = 0.3f;
        initializeEnemyMind(memory, way, 37);
        CHECK(memory.effectiveWay == way);
        CHECK(memory.primed);
        CHECK(memory.heading == 0.4f);
        CHECK(memory.headingBefore == 0.3f);
        if (way == kLungeWay) {
            CHECK(memory.counter == 7);
        } else {
            CHECK(memory.fuse == (way == kCastWay ? 97 : 7));
        }
    }
}

TEST_CASE("unseen wandering does not carry a collision hold into a reacquired target",
          "[game][enemies][mind][mind-transition]") {
    const auto& seek = enemyMindOf(kSeekWay);
    MindMemory memory;
    MindSense unseen;
    unseen.wanderClear = [](f32) { return false; };
    seek.think(memory, unseen);
    REQUIRE(memory.deadEnd == 20);
    memory.headingBefore = memory.heading;
    const auto found = seek.think(memory, senseAhead());
    CHECK(memory.deadEnd == 0);
    CHECK(found.heading == Approx(0).margin(0.000001));
}

TEST_CASE(
    "ranged casters restart their entry delay after a close chase without resetting each tick",
    "[game][enemies][mind][mind-transition]") {
    for (const s32 way : {kStandCastWay, kRangeCastWay}) {
        MindMemory memory;
        const auto& mind = enemyMindOf(way);
        auto distant = senseAhead(12);
        distant.random = 9;
        mind.think(memory, distant);
        CHECK(memory.fuse == 7);
        mind.think(memory, senseAhead(4));
        const auto resumed = mind.think(memory, distant);
        CHECK(resumed.action == EnemyAction::Ready);
        CHECK(memory.fuse == 7);
        mind.think(memory, distant);
        CHECK(memory.fuse == 5);
    }
}

TEST_CASE(
    "temporary pursuits preserve shared range and zig-zag flags while resetting entry counters",
    "[game][enemies][mind][mind-transition]") {
    // format_brain preserves flag1/mode1 for 29, and flag1/flag2/mode1 for 14.
    const auto& range = enemyMindOf(kRangeCastWay);
    MindMemory keeper;
    initializeEnemyMind(keeper, kRangeCastWay, 0);
    range.think(keeper, senseAhead(7));
    REQUIRE(keeper.mode == 1);
    range.think(keeper, senseAhead(4));
    range.think(keeper, senseAhead(9));
    CHECK(keeper.mode == 1); // still backing off until the ten-unit boundary

    const auto& zig = enemyMindOf(kZigZagWay);
    MindMemory memory;
    initializeEnemyMind(memory, kZigZagWay, 0);
    memory.zigZag = {.count = 10, .side = -1, .spread = 3, .hold = 20, .swings = 2};
    zig.think(memory, senseAhead(4));
    REQUIRE(memory.effectiveWay == kSeekWay);
    zig.think(memory, senseAhead(20));
    CHECK(memory.effectiveWay == kZigZagWay);
    CHECK(memory.zigZag.count == 43); // reentry resets count, so a new quarter-turn occurs
    CHECK(memory.zigZag.hold == 0);
    CHECK(memory.zigZag.side == 1);
    CHECK(memory.zigZag.spread == 3);
    CHECK(memory.zigZag.swings == 3);
}

TEST_CASE("the chase goes straight for its player, and round a corner the nearer way, further "
          "round with every bump",
          "[game][enemies][mind]") {
    const EnemyMind& chase = enemyMindOf(kChaseWay);
    MindMemory memory;
    // Straight at a player off to the right: the heading is theirs, the body turns, walks.
    MindSense sense = senseAhead();
    sense.targetPosition = Vec3{10.0f, 0.0f, 10.0f};
    MindIntent intent = chase.think(memory, sense);
    REQUIRE(intent.heading == Approx(kPi / 4.0f));
    REQUIRE(intent.turn);
    REQUIRE(intent.pace == 1.0f);
    REQUIRE(intent.action == EnemyAction::Walk);
    REQUIRE_FALSE(intent.become.has_value());
    REQUIRE(memory.heading == Approx(kPi / 4.0f));
    // Nobody seen: it wanders on the way it was going.
    MindSense blind = senseAhead();
    blind.recognized = false;
    intent = chase.think(memory, blind);
    REQUIRE(intent.heading == Approx(kPi / 4.0f));
    // Reacquiring the target starts a fresh Chase before testing a later Chase collision.
    chase.think(memory, sense);
    REQUIRE(memory.collided == 0);
    REQUIRE(memory.deadEnd == 0);
    // A dead stop against a wall: the heading is held ten ticks, one bump counted; against
    // another enemy, fifteen.
    sense = senseAhead();
    sense.targetPosition = Vec3{0.0f, 0.0f, 20.0f};
    sense.bumpedWall = true;
    sense.blocked = true;
    intent = chase.think(memory, sense);
    REQUIRE(memory.collided == 1);
    REQUIRE(memory.deadEnd == 10 - 2);
    REQUIRE(intent.heading == Approx(kPi / 4.0f)); // held
    for (s32 i = 0; i < 5; ++i) {
        intent = chase.think(memory, senseAhead());
    }
    REQUIRE(memory.deadEnd <= 0);
    MindMemory jostled;
    MindSense other = senseAhead();
    other.bumpedOther = true;
    other.blocked = true;
    other.otherSide = -1;
    chase.think(jostled, other);
    REQUIRE(jostled.deadEnd == 15 - 2);
    REQUIRE(jostled.route == -1);
    // Straight into a wall (the straight step not open), it goes round on the side the
    // nearer probe gives (the player dead ahead and the body facing a little left, the right
    // probe ends nearer), a sixteenth of a turn further from straight until a step is open.
    sense = senseAhead();
    sense.yaw = -0.3f;
    sense.open = [](f32 heading) { return heading > 0.5f; };
    intent = chase.think(memory, sense);
    REQUIRE(memory.route == 1);
    REQUIRE(memory.skirting);
    REQUIRE(intent.heading == Approx(kPi / 4.0f)); // the first open sixteenth past 0.5
    // Only the other side open: it takes that, and the route with it.
    sense.open = [](f32 heading) { return heading < -1.0f; };
    intent = chase.think(memory, sense);
    REQUIRE(memory.route == -1);
    REQUIRE(intent.heading == Approx(-3.0f * kPi / 8.0f));
    // The straight way open again, it goes straight and skirts no more.
    sense.open = nullptr;
    intent = chase.think(memory, sense);
    REQUIRE(intent.heading == 0.0f);
    REQUIRE_FALSE(memory.skirting);
    // Seven bumps and the route doubles back the other way.
    sense = senseAhead();
    sense.blocked = true;
    memory.route = 1;
    memory.collided = 0;
    for (s32 b = 0; b < 7; ++b) {
        chase.think(memory, sense);
        for (s32 i = 0; i < 6; ++i) {
            chase.think(memory, senseAhead());
        }
    }
    REQUIRE(memory.route == -2);
    REQUIRE(memory.collided == 0);
    // A doubled route that bumps again is given up: a long hold and a fresh start.
    memory.route = -4;
    memory.deadEnd = 0;
    chase.think(memory, sense);
    REQUIRE(memory.deadEnd == 60 - 2);
    REQUIRE(memory.route == 0);
    REQUIRE(memory.collided == 0);
}

TEST_CASE("the chase refuses headings that lead into things or straight back, and goes straight "
          "anyway after ten refusals",
          "[game][enemies][mind]") {
    const EnemyMind& chase = enemyMindOf(kChaseWay);
    MindMemory memory;
    MindSense sense = senseAhead();
    sense.clear = [](f32) { return false; };
    MindIntent intent = chase.think(memory, sense);
    REQUIRE(memory.stuck == 1);
    REQUIRE(intent.heading == 0.0f);
    REQUIRE(intent.turn); // straight at the player is always faced
    // A refused heading is not taken up by the memory.
    memory.heading = 1.0f;
    sense.targetPosition = Vec3{10.0f, 0.0f, 0.0f};
    intent = chase.think(memory, sense);
    REQUIRE(memory.heading == 1.0f);
    REQUIRE(memory.stuck == 2);
    // Nothing open anywhere: refused, and straight anyway.
    MindMemory walled;
    walled.heading = 1.0f;
    MindSense shut = senseAhead();
    shut.open = [](f32) { return false; };
    intent = chase.think(walled, shut);
    REQUIRE(walled.stuck == 1);
    REQUIRE(walled.heading == 1.0f);
    REQUIRE(intent.heading == 0.0f);
    for (s32 i = 0; i < 10; ++i) {
        chase.think(memory, sense);
    }
    REQUIRE(memory.stuck > 10);
    REQUIRE(memory.heading == Approx(kPi / 2.0f)); // straight anyway
    // Turning straight back to the heading just left is refused too.
    MindMemory turning;
    turning.heading = 0.5f;
    turning.headingBefore = 0.0f;
    MindSense back = senseAhead();
    back.targetPosition = Vec3{0.0f, 0.0f, 20.0f}; // straight ahead is the heading before
    chase.think(turning, back);
    REQUIRE(turning.stuck == 1);
    REQUIRE(turning.heading == 0.5f);
}

TEST_CASE("the seek sweeps its route side, the prowler pounces within eight, "
          "and the wanderer turns at dead ends",
          "[game][enemies][mind]") {
    const EnemyMind& seek = enemyMindOf(kSeekWay);
    MindMemory memory;
    memory.route = 1;
    MindSense sense = senseAhead();
    // Straight is blocked, right is clear: a sixteenth right is taken.
    sense.clear = [](f32 heading) { return heading > 0.1f; };
    MindIntent intent = seek.think(memory, sense);
    REQUIRE(intent.heading == Approx(kPi / 8.0f));
    // Only the left is clear, and only well round.
    memory.route = -1;
    sense.clear = [](f32 heading) { return heading < -1.0f; };
    intent = seek.think(memory, sense);
    REQUIRE(intent.heading == Approx(-3.0f * kPi / 8.0f));
    // Nothing clear: straight anyway.
    sense.clear = [](f32) { return false; };
    intent = seek.think(memory, sense);
    REQUIRE(intent.heading == 0.0f);
    // The prowler goes straight on, and seeks only while a player is within eight (its
    // crowded distance), going back to its business once they are further.
    const EnemyMind& prowl = enemyMindOf(kProwlWay);
    MindMemory rat;
    rat.heading = 1.0f;
    rat.headingBefore = rat.heading;
    intent = prowl.think(rat, senseAhead(20.0f));
    REQUIRE(intent.heading == 1.0f);
    REQUIRE_FALSE(intent.become.has_value());
    intent = prowl.think(rat, senseAhead(7.0f));
    REQUIRE(intent.heading == 0.0f);
    REQUIRE_FALSE(intent.become.has_value());
    MindSense crowded = senseAhead(7.0f);
    crowded.closeDistance = 9.0f; // others after the same player keep it off
    rat.heading = 1.0f;
    CHECK(prowl.think(rat, crowded).heading == 1.0f);
    CHECK(enemyMindOf(kSeekAliasWay).name() == "seek");
    // A wanderer's probe turns a quarter left, then the twenty-tick hold turns it again.
    const EnemyMind& wander = enemyMindOf(kWanderWay);
    MindMemory roaming;
    roaming.heading = 0.0f;
    MindSense bump;
    bump.ticks = 2;
    bump.bumpedWall = true;
    bump.wanderClear = [](f32) { return false; };
    intent = wander.think(roaming, bump);
    REQUIRE(intent.heading == Approx(-kPi / 2.0f));
    REQUIRE(roaming.turns == 0);
    REQUIRE(roaming.deadEnd == 20);
    bump.wanderClear = [](f32) { return true; };
    intent = wander.think(roaming, bump);
    REQUIRE(intent.heading == Approx(-kPi / 2.0f)); // held
    REQUIRE(roaming.turns == 0);
    MindSense touching;
    touching.contact = 0;
    touching.contactPosition = Vec3{5.0f, 0.0f, 0.0f};
    intent = wander.think(roaming, touching);
    REQUIRE(intent.heading == Approx(-kPi / 2.0f));
}

TEST_CASE("a prowler stopped waits, turns an eighth to its own side, and the fourth turn "
          "changes it to the mirrored way",
          "[game][enemies][mind]") {
    const EnemyMind& prowl = enemyMindOf(kProwlWay);
    const EnemyMind& mirrored = enemyMindOf(kMirroredProwlWay);
    MindMemory rat;
    MindSense alone;
    alone.ticks = 2;
    MindSense stopped = alone;
    stopped.blocked = true;
    stopped.bumpedWall = true;
    std::optional<s32> became;
    for (s32 turn = 1; turn <= 4; ++turn) {
        // Held after the stop: thirty ticks, fifteen updates of two.
        prowl.think(rat, stopped);
        CHECK(rat.deadEnd > 0);
        MindIntent intent;
        for (s32 i = 0; i < 15 && rat.deadEnd > 0; ++i) {
            intent = prowl.think(rat, alone);
        }
        CHECK(rat.deadEnd <= 0);
        CHECK(intent.heading == Approx(wrapAngle(static_cast<f32>(turn) * kPi / 4.0f)));
        became = intent.become;
        CHECK(became.has_value() == (turn == 4));
    }
    REQUIRE(became == kMirroredProwlWay);
    CHECK(rat.counter == 0);
    // The mirrored way turns the other way, and back again after four.
    MindMemory other;
    mirrored.think(other, stopped);
    MindIntent intent;
    for (s32 i = 0; i < 15 && other.deadEnd > 0; ++i) {
        intent = mirrored.think(other, alone);
    }
    CHECK(intent.heading == Approx(-kPi / 4.0f));
    // A player against it is faced.
    MindSense touching = alone;
    touching.contact = 0;
    touching.contactPosition = Vec3{5.0f, 0.0f, 0.0f};
    CHECK(prowl.think(rat, touching).heading == Approx(kPi / 2.0f));
}

TEST_CASE("the skirmisher waits out its throw before it throws or backs off, is nudged at "
          "stops and gives up after eight",
          "[game][enemies][mind]") {
    const EnemyMind& skirmish = enemyMindOf(kSkirmishWay);
    MindMemory archer;
    const MindSense close = senseAhead(10.0f); // within six tenths of thirty
    archer.fuse = 20;
    MindIntent intent = skirmish.think(archer, close);
    CHECK(archer.keepingOff);
    CHECK(intent.pace == 0.0f); // the wait runs first
    for (s32 i = 0; i < 9; ++i) {
        intent = skirmish.think(archer, close);
        CHECK(intent.pace == 0.0f);
    }
    intent = skirmish.think(archer, close);
    CHECK(intent.pace == Approx(0.8f));
    CHECK(std::abs(intent.heading) == Approx(kPi));
    CHECK(intent.action == EnemyAction::RunAttack);
    REQUIRE(intent.facing.has_value());
    CHECK(*intent.facing == Approx(0.0f));
    // Stopped, it is nudged off straight a step at a time; the ninth stop gives it up.
    MindSense stopped = close;
    stopped.blocked = true;
    intent = skirmish.think(archer, stopped);
    CHECK(std::abs(wrapAngle(intent.heading - kPi)) == Approx(0.0872664601f));
    for (s32 i = 0; i < 7; ++i) {
        skirmish.think(archer, stopped);
    }
    CHECK(archer.keepingOff);
    intent = skirmish.think(archer, stopped);
    CHECK_FALSE(archer.keepingOff);
    CHECK(intent.throwing); // standing its ground, it throws again
}

TEST_CASE("a range keeper stopped by the world attacks where it stands instead of stepping",
          "[game][enemies][mind]") {
    const EnemyMind& range = enemyMindOf(kRangeCastWay);
    MindMemory caster;
    caster.primed = true;
    caster.fuse = 0;
    MindSense close = senseAhead(7.0f); // inside eight: backing off
    close.random = 0;
    MindIntent intent = range.think(caster, close);
    REQUIRE(caster.mode == 1);
    CHECK(intent.pace == Approx(0.8f));
    MindSense walled = close;
    walled.bumpedWall = true;
    intent = range.think(caster, walled);
    CHECK(intent.pace == 0.0f);
}

TEST_CASE("the loiterer turns on the spot until its generator is gone, the fleer runs away, the "
          "lurker waits to be seen, and the stander stands",
          "[game][enemies][mind]") {
    const EnemyMind& loiter = enemyMindOf(kLoiterWay);
    MindMemory memory;
    MindSense sense;
    sense.ticks = 2;
    MindIntent intent = loiter.think(memory, sense);
    REQUIRE(intent.pace == 0.0f);
    REQUIRE(intent.action == EnemyAction::Ready);
    REQUIRE(intent.heading == Approx(2.0f * kPi / 180.0f)); // a degree a tick
    REQUIRE_FALSE(intent.expire);
    sense.generatorGone = true;
    REQUIRE(loiter.think(memory, sense).expire);
    // The fleer runs straight away from the lit bomber at twice its pace, nudged five degrees
    // more off straight for each step in a row that gets nowhere.
    const EnemyMind& flee = enemyMindOf(kFleeWay);
    MindMemory fleeing;
    MindSense scared = senseAhead();
    scared.bomber = Vec3{0.0f, 0.0f, 3.0f};
    intent = flee.think(fleeing, scared);
    REQUIRE(std::abs(intent.heading) == Approx(kPi));
    REQUIRE(intent.pace == 2.0f);
    REQUIRE(intent.action == EnemyAction::Run);
    scared.blocked = true;
    const f32 fiveDegrees = kPi / 36.0f;
    REQUIRE(wrapAngle(flee.think(fleeing, scared).heading - kPi) == Approx(fiveDegrees));
    REQUIRE(wrapAngle(flee.think(fleeing, scared).heading - kPi) == Approx(-fiveDegrees));
    REQUIRE(wrapAngle(flee.think(fleeing, scared).heading - kPi) == Approx(2.0f * fiveDegrees));
    scared.blocked = false;
    REQUIRE(std::abs(flee.think(fleeing, scared).heading) == Approx(kPi));
    MindSense alone;
    alone.ticks = 2;
    fleeing.heading = 0.7f;
    intent = flee.think(fleeing, alone);
    REQUIRE(intent.heading == 0.7f); // no bomber to flee: wandering
    // The ways whose move_logic looks out for bombers; the throwers and the bomber do not.
    for (const s32 way : {kSeekWay, kProwlWay, kWanderWay, kChaseWay, kSkirmishWay, 30}) {
        CHECK(fleesBombers(way));
    }
    for (const s32 way : {kThrowWay, kBombWay, kSuicideWay, kLurkWay, kLoiterWay, kLungeWay}) {
        CHECK_FALSE(fleesBombers(way));
    }
    // The lurker is still until a player is within sight, then seeks for good.
    const EnemyMind& lurk = enemyMindOf(kLurkWay);
    MindMemory lurking;
    MindSense far = senseAhead(40.0f);
    far.sight = 30.0f;
    intent = lurk.think(lurking, far);
    REQUIRE(intent.pace == 0.0f);
    REQUIRE_FALSE(intent.turn);
    REQUIRE_FALSE(lurking.woken);
    intent = lurk.think(lurking, senseAhead(25.0f));
    REQUIRE(lurking.woken);
    REQUIRE(intent.pace == 1.0f);
    REQUIRE(intent.become == kSeekWay);
}

TEST_CASE("the zig-zagger swings a quarter turn at a time and aims afresh once it drifts",
          "[game][enemies][mind]") {
    const EnemyMind& zig = enemyMindOf(kZigZagWay);
    REQUIRE(zig.name() == "zig-zag");
    MindMemory memory;
    const MindSense sense = senseAhead(20.0f); // the player straight ahead, along +z
    // Its first tick swings it a quarter turn, always the same way, and it walks on that way.
    const MindIntent intent = zig.think(memory, sense);
    zig.afterMove(memory, sense, sense);
    CHECK(intent.heading == Approx(-kPi / 2.0f));
    CHECK(intent.pace == 1.0f);
    CHECK(memory.zigZag.count == 43);
    // Every 45 ticks another. Four bring it back to straight at the player; once it has made
    // four and drifted over a quarter turn off (the sixth), it aims afresh an eighth of a
    // turn off straight at them, and holds that aim thirty ticks.
    s32 ticks = 2;
    for (; ticks < 600 && memory.zigZag.hold != 30; ticks += 2) {
        zig.think(memory, sense);
        zig.afterMove(memory, sense, sense);
    }
    CHECK(ticks == 226); // the sixth swing, 43 ticks after the first and 45 apart
    CHECK(memory.zigZag.swings == 0);
    CHECK(memory.zigZag.spread == 1);
    CHECK(memory.heading == Approx(-kPi / 4.0f));
    // Within eight it seeks; unseen it wanders.
    MindMemory close;
    CHECK(zig.think(close, senseAhead(7.0f)).heading == Approx(0.0f));
    MindMemory lost;
    lost.heading = 0.6f;
    MindSense alone;
    alone.ticks = 2;
    CHECK(zig.think(lost, alone).heading == 0.6f);
}

TEST_CASE("generator zig-zag births alternate their swings instead of circling",
          "[game][enemies][mind][courtyard-grunt]") {
    const auto& zig = enemyMindOf(kZigZagWay);
    for (const s32 side : {-1, 1}) {
        MindMemory memory;
        memory.zigZag.side = side;
        memory.heading = static_cast<f32>(side) * kPi / 4;
        MindSense sense = senseAhead(20);
        sense.ticks = 1;
        for (s32 tick = 0; tick < 180; ++tick) {
            const auto intent = zig.think(memory, sense);
            zig.afterMove(memory, sense, sense);
            // The first swing reverses the birth offset, then alternates either side
            // of the generator's forward direction, never turning away from its player.
            const s32 swing = tick == 0 ? 0 : (tick + 1) / 45;
            const f32 expected = static_cast<f32>((swing % 2 == 0 ? -1 : 1) * side) * kPi / 4;
            CHECK(intent.heading == Approx(expected));
        }
    }
}

TEST_CASE("zig-zaggers use crowded distance when changing to a close approach",
          "[game][enemies][mind][zigzag-parity]") {
    // move_logic14 0x8004A108 reads close_dist (+0x278), not actual_dist (+0x27c).
    const EnemyMind& zig = enemyMindOf(kZigZagWay);
    MindSense sense = senseAhead(7);
    sense.closeDistance = 9;
    MindMemory crowded;
    crowded.heading = kPi / 4;
    crowded.headingBefore = crowded.heading;
    crowded.zigZag.count = 10;
    CHECK(zig.think(crowded, sense).heading == Approx(kPi / 4));
    CHECK(crowded.zigZag.count == 8);

    // Once the crowd clears, an equally close player receives the direct Seek approach.
    sense.closeDistance = 8;
    MindMemory clear;
    clear.heading = kPi / 4;
    clear.headingBefore = clear.heading; // no abandoned heading for Seek's anti-backtrack guard
    clear.zigZag.count = 10;
    CHECK(zig.think(clear, sense).heading == Approx(0).margin(0.000001));
    CHECK(clear.zigZag.count == 10);
}

TEST_CASE("zig-zaggers judge drift against a decoy but reseed toward the actual player",
          "[game][enemies][mind][zigzag-parity]") {
    // move_logic14 0x8004A170 uses Mikey's +0x9e4 position for facing, but 0x8004A360
    // selects the player's +0x44 position unconditionally when calculating a fresh aim.
    const EnemyMind& zig = enemyMindOf(kZigZagWay);
    MindSense sense = senseAhead();
    sense.targetPlayerPosition = Vec3{0, 0, 20};
    sense.targetPosition = {20, 0, 0};
    MindMemory memory;
    memory.heading = kPi / 8;
    memory.zigZag.count = 10;
    memory.zigZag.side = 1;

    SECTION("a blocked step reseeds around the body rather than its decoy") {
        sense.blocked = true;
        const auto intent = zig.think(memory, sense);
        CHECK(memory.heading == Approx(kPi / 8));
        zig.afterMove(memory, sense, sense);
        // The current step is taken before the fresh aim applies to the following tick.
        CHECK(intent.heading == Approx(kPi / 8));
        CHECK(memory.heading == Approx(kPi / 4));
        CHECK(memory.zigZag.count == 0);
        CHECK(memory.zigZag.hold == 30);
    }

    SECTION("a decoy behind the current heading can trigger the four-swing reseed") {
        memory.heading = 0;
        memory.zigZag.swings = 4;
        sense.targetPosition = {0, 0, -20};
        CHECK(zig.think(memory, sense).heading == Approx(0).margin(0.000001));
        zig.afterMove(memory, sense, sense);
        CHECK(memory.heading == Approx(-kPi / 4));
        CHECK(memory.zigZag.swings == 0);
        CHECK(memory.zigZag.hold == 30);
    }

    SECTION("a decoy ahead suppresses reseeding even when the real player is behind") {
        memory.heading = 0;
        memory.zigZag.swings = 4;
        sense.targetPosition = {0, 0, 20};
        sense.targetPlayerPosition = Vec3{0, 0, -20};
        zig.think(memory, sense);
        zig.afterMove(memory, sense, sense);
        CHECK(memory.heading == Approx(0).margin(0.000001));
        CHECK(memory.zigZag.swings == 4);
        CHECK(memory.zigZag.hold == -sense.ticks);
    }
}

TEST_CASE("the ranged casters wait, then attack from where they stand or keep their distance",
          "[game][enemies][mind]") {
    const EnemyMind& stand = enemyMindOf(kStandCastWay);
    const EnemyMind& range = enemyMindOf(kRangeCastWay);
    REQUIRE(stand.name() == "stand-cast");
    REQUIRE(range.name() == "range-cast");
    // Its first wait is drawn at random under thirty ticks, then it attacks: the power attack
    // at strength three, else the two attacks in turn.
    MindMemory memory;
    MindSense sense = senseAhead(12.0f);
    sense.random = 4;
    sense.tier = 3;
    MindIntent intent = stand.think(memory, sense);
    CHECK(intent.pace == 0.0f);
    CHECK(intent.action == EnemyAction::Ready);
    CHECK(memory.fuse == 2);
    intent = stand.think(memory, sense);
    CHECK(intent.action == EnemyAction::Ready);
    CHECK(stand.think(memory, sense).action == EnemyAction::PowerAttack);
    sense.tier = 2;
    CHECK(stand.think(memory, sense).action == EnemyAction::Attack);
    CHECK(stand.think(memory, sense).action == EnemyAction::Attack2);
    // Not above or below by more than ten, and within six it chases hand to hand.
    MindSense high = sense;
    high.targetVertical = 11.0f;
    CHECK(stand.think(memory, high).action == EnemyAction::Ready);
    CHECK(stand.think(memory, senseAhead(5.0f)).pace > 0.0f);
    // The range keeper backs off from within eight, still facing, until beyond ten; closes in
    // from beyond eighteen until within sixteen; between, it stands and attacks.
    MindMemory keeper;
    MindSense close = senseAhead(7.0f);
    close.random = 0;
    intent = range.think(keeper, close);
    CHECK(keeper.mode == 1);
    CHECK(intent.pace == 0.8f);
    CHECK(std::abs(intent.heading) == Approx(kPi));
    CHECK_FALSE(intent.turn);
    const MindSense clear = senseAhead(11.0f);
    intent = range.think(keeper, clear);
    CHECK(keeper.mode == 0);
    CHECK(intent.pace == 0.0f);
    const MindSense far = senseAhead(20.0f);
    intent = range.think(keeper, far);
    CHECK(keeper.mode == 2);
    CHECK(intent.heading == Approx(0.0f));
    CHECK(intent.pace == 0.8f);
    CHECK(intent.turn);
    CHECK(range.think(keeper, senseAhead(15.0f)).action != EnemyAction::Walk);
    CHECK(keeper.mode == 0);
}

TEST_CASE("zig-zag completion uses the moved position and only the handler that actually ran",
          "[game][enemies][mind][zigzag-parity]") {
    const auto& zig = enemyMindOf(kZigZagWay);
    MindMemory memory;
    memory.heading = kPi / 8;
    memory.zigZag.count = 10;
    memory.zigZag.side = 1;
    auto before = senseAhead();
    before.blocked = true; // last update's blockage must not cause this update's reseed
    zig.think(memory, before);
    CHECK(memory.heading == Approx(kPi / 8));
    auto after = before;
    after.position = {4, 0, 0};
    after.blocked = false;
    zig.afterMove(memory, before, after);
    CHECK(memory.heading == Approx(kPi / 8));
    CHECK(memory.zigZag.hold == -2);

    zig.think(memory, before);
    after.blocked = true;
    zig.afterMove(memory, before, after);
    CHECK(memory.heading == Approx(std::atan2(-4.0f, 20.0f) + kPi / 4));
    CHECK(memory.zigZag.hold == 30);

    auto close = senseAhead(4);
    zig.think(memory, close);
    const auto held = memory.zigZag;
    close.blocked = true;
    zig.afterMove(memory, close, close);
    CHECK(memory.effectiveWay == kSeekWay);
    CHECK(memory.zigZag.count == held.count);
    CHECK(memory.zigZag.hold == held.hold);
    CHECK(memory.zigZag.swings == held.swings);
}

TEST_CASE("the patroller walks the lookouts until a player comes near", "[game][enemies][mind]") {
    std::vector<WorldLocator> locators(4);
    locators[0].kind = LocatorKind::Sentry;
    locators[0].position = Vec3{0, 0, 10};
    locators[0].next = 1;
    locators[1].kind = LocatorKind::CameraGame; // not a lookout
    locators[2].kind = LocatorKind::Sentry;
    locators[2].position = Vec3{10, 0, 10};
    locators[2].next = 0;
    locators[3].kind = LocatorKind::Event;
    locators[3].position = Vec3{50, 0, 50};
    locators[3].next = 3;
    const LookoutRoute route = LookoutRoute::of(locators);
    REQUIRE(route.points.size() == 3);
    CHECK(route.next == std::vector<s32>{1, 0, 3});
    const EnemyMind& patrol = enemyMindOf(kPatrolWay);
    REQUIRE(patrol.name() == "patrol");
    MindMemory memory;
    MindSense sense;
    sense.ticks = 2;
    sense.lookouts = &route;
    // To the nearest lookout first, at a walk.
    const MindIntent intent = patrol.think(memory, sense);
    CHECK(memory.lookout == 0);
    CHECK(intent.heading == Approx(0.0f));
    CHECK(intent.pace == 1.0f);
    CHECK(intent.action == EnemyAction::Walk);
    // Reached, on to the one it names.
    sense.position = Vec3{0.5f, 1.0f, 9.6f};
    patrol.think(memory, sense);
    CHECK(memory.lookout == 1);
    CHECK(patrol.think(memory, sense).heading == Approx(kPi / 2.0f).margin(0.05));
    // A player within four fifths of its sight: it seeks them, and takes up the round again
    // from the nearest once they are further off.
    MindSense seen = senseAhead(20.0f);
    seen.sight = 30.0f;
    seen.lookouts = &route;
    patrol.think(memory, seen);
    CHECK(memory.lookout == -1);
    seen = senseAhead(28.0f);
    seen.sight = 30.0f;
    seen.lookouts = &route;
    patrol.think(memory, seen);
    CHECK(memory.lookout == 0);
    // No lookouts: it wanders.
    MindMemory lost;
    lost.heading = 0.3f;
    MindSense bare;
    bare.ticks = 2;
    CHECK(patrol.think(lost, bare).heading == 0.3f);
}

TEST_CASE("the caster seeks and casts on its wait, wanders unseen, and fights close by",
          "[game][enemies][mind]") {
    const EnemyMind& cast = enemyMindOf(kCastWay);
    REQUIRE(cast.name() == "cast");
    MindMemory memory;
    memory.primed = true; // the initial casting hold has elapsed
    MindSense sense = senseAhead(20.0f);
    sense.tier = 3;
    sense.castWait = 90;
    sense.random = 3; // a window of 23 ticks, and a wait of 23 + 90
    // The first thing it does on seeing its player is cast, the power attack at strength
    // three, while it goes on towards them.
    MindIntent intent = cast.think(memory, sense);
    REQUIRE(intent.action == EnemyAction::PowerAttack);
    REQUIRE(intent.heading == Approx(0.0f));
    REQUIRE(memory.deadEnd == 0); // move_logic30's Seek delegate reformats against prev_ai=30
    REQUIRE(memory.fuse == 23 + 90);
    // The animation owns completion of the requested attack; AI does not request it for
    // an invented 23-tick window after the delegate has cleared dead_end.
    intent = cast.think(memory, sense);
    REQUIRE(intent.action == EnemyAction::Walk);
    REQUIRE(memory.fuse == 23 + 90 - sense.ticks);
    MindMemory weak;
    weak.primed = true;
    sense.tier = 1;
    REQUIRE(cast.think(weak, sense).action == EnemyAction::Attack);
    // Unseen it wanders; within six it chases, hand to hand.
    MindMemory idle;
    MindSense alone;
    alone.ticks = 2;
    idle.heading = 0.4f;
    REQUIRE(cast.think(idle, alone).heading == 0.4f);
    MindMemory close;
    REQUIRE(cast.think(close, senseAhead(5.0f)).action == EnemyAction::Walk);
    REQUIRE(close.fuse == 0); // no cast begun
}

TEST_CASE("the lunger creeps up facing its player and lunges or makes its power attack",
          "[game][enemies][mind]") {
    const EnemyMind& lunge = enemyMindOf(kLungeWay);
    MindMemory memory;
    memory.primed = true; // the initial lunge hold has elapsed
    // Nobody to face: it stands.
    MindSense alone;
    alone.ticks = 2;
    MindIntent intent = lunge.think(memory, alone);
    REQUIRE(intent.pace == 0.0f);
    // Near enough, the first thing it does is lunge; further off, its power attack.
    MindSense sense = senseAhead(8.0f);
    sense.ticks = 2;
    sense.recognized = true;
    sense.targetPosition = Vec3{8.0f, 0.0f, 0.0f};
    sense.random = 7;
    intent = lunge.think(memory, sense);
    REQUIRE(intent.action == EnemyAction::Attack);
    REQUIRE(intent.heading == Approx(kPi / 2.0f));
    REQUIRE(intent.pace == 0.0f);
    MindMemory far;
    far.primed = true;
    MindSense distant = sense;
    distant.targetDistance = 12.0f;
    REQUIRE(lunge.think(far, distant).action == EnemyAction::PowerAttack);
    // Lunging it goes at full pace, and lands only within seven and a half.
    sense.action = EnemyAction::Attack;
    intent = lunge.think(memory, sense);
    REQUIRE(intent.pace == 1.0f);
    REQUIRE_FALSE(intent.strike);
    REQUIRE(memory.counter == 30 + 7);
    sense.targetDistance = 7.0f;
    REQUIRE(lunge.think(memory, sense).strike);
    // Then it creeps on at half pace until the wait is out.
    sense.action = EnemyAction::Walk;
    for (s32 tick = 0; tick < 37; tick += 2) {
        intent = lunge.think(memory, sense);
        REQUIRE(intent.pace == 0.5f);
        REQUIRE(intent.action == EnemyAction::Walk);
    }
    REQUIRE(lunge.think(memory, sense).action == EnemyAction::Attack);
}

TEST_CASE("a seek sweep reaches behind its route side without reversing its last heading",
          "[game][enemies][mind][ai-parity]") {
    // move_logic00 (0x80046B54), table 0x8011C0C4: zero, then eight cumulative pi/8 steps.
    const EnemyMind& seek = enemyMindOf(kSeekWay);
    for (const s32 side : {-1, 1}) {
        MindMemory memory;
        memory.route = side;
        MindSense sense = senseAhead();
        std::vector<f32> probes;
        sense.clear = [&](f32 heading) {
            probes.push_back(heading);
            return static_cast<f32>(side) * heading > kPi / 2;
        };
        const MindIntent intent = seek.think(memory, sense);
        CHECK(intent.heading == Approx(static_cast<f32>(side) * 5 * kPi / 8));
        REQUIRE(probes.size() == 6);
        for (usize i = 0; i < probes.size(); ++i) {
            CHECK(probes[i] == Approx(static_cast<f32>(side) * static_cast<f32>(i) * kPi / 8));
        }
        CHECK(memory.collided == 5);
    }

    SECTION("a just-abandoned bearing and a near reversal are not retried") {
        MindMemory memory;
        memory.route = 1;
        memory.heading = kPi / 8;
        memory.headingBefore = 0;
        CHECK(seek.think(memory, senseAhead()).heading == Approx(kPi / 8));
        CHECK(memory.headingBefore == Approx(kPi / 8));

        memory.heading = kPi;
        memory.headingBefore = kPi;
        CHECK(seek.think(memory, senseAhead()).heading == Approx(kPi / 8));
    }
}

TEST_CASE("patrollers ignore disconnected lookouts and retain crowding in their chase threshold",
          "[game][enemies][mind][ai-parity]") {
    // move_logic15 (0x8004A430): next >= 0 qualifies a starting node, close_dist gates chase.
    const EnemyMind& patrol = enemyMindOf(kPatrolWay);
    LookoutRoute route;
    route.points = {{2, 0, 0}, {0, 0, 10}, {10, 0, 10}};
    route.next = {-1, 2, 1};
    MindMemory memory;
    MindSense sense;
    sense.lookouts = &route;
    CHECK(patrol.think(memory, sense).heading == Approx(0));
    CHECK(memory.lookout == 1);

    sense = senseAhead(23);
    sense.closeDistance = 25;
    sense.lookouts = &route;
    patrol.think(memory, sense);
    CHECK(memory.lookout == 1);
    sense.closeDistance = 23;
    patrol.think(memory, sense);
    CHECK(memory.lookout == -1);

    SECTION("all disconnected nodes safely fall back to wandering") {
        route.next = {-1, -1, -1};
        memory = {};
        memory.heading = 0.3f;
        sense.target = -1;
        CHECK(patrol.think(memory, sense).heading == Approx(0.3f));
        CHECK(memory.lookout == -1);
    }
}

TEST_CASE("a ranged caster tracks its player while backing off or nudging around a block",
          "[game][enemies][mind][ai-parity]") {
    // move_logic29 (0x8004BF9C) always turns toward ang, independently of its translation.
    MindMemory memory;
    memory.primed = true;
    MindSense sense = senseAhead(7);
    const EnemyMind& range = enemyMindOf(kRangeCastWay);
    range.think(memory, sense);
    sense.targetPosition = {7, 0, 0};
    sense.blocked = true;
    const auto intent = range.think(memory, sense);
    REQUIRE(intent.facing.has_value());
    CHECK(*intent.facing == Approx(kPi / 2));
    CHECK(intent.heading != Approx(*intent.facing));
    CHECK(intent.pace == Approx(0.8f));
}

TEST_CASE("casters and Garm brood respect their initial holds and planted attack frames",
          "[game][enemies][mind][ai-parity]") {
    // format_brain (0x80050394): way 30 waits 60+RandInt(60); way 31 RandInt(30).
    MindSense sense = senseAhead(20);
    sense.ticks = 1;
    sense.random = 7;
    MindMemory caster;
    const EnemyMind& cast = enemyMindOf(kCastWay);
    for (s32 tick = 0; tick < 66; ++tick) {
        CHECK(cast.think(caster, sense).action == EnemyAction::Walk);
    }
    CHECK(cast.think(caster, sense).action == EnemyAction::Attack);

    const EnemyMind& lunge = enemyMindOf(kLungeWay);
    MindMemory brood;
    for (s32 tick = 0; tick < 7; ++tick) {
        const auto intent = lunge.think(brood, sense);
        CHECK(intent.action == EnemyAction::Walk);
        CHECK(intent.pace == Approx(0.5f));
    }
    auto intent = lunge.think(brood, sense);
    CHECK(intent.action == EnemyAction::PowerAttack);
    CHECK(intent.pace == 0);
    // move_logic31 (0x8004C650) updates velocity only for actions 12/13 and the wait branch.
    for (const auto action : {EnemyAction::PowerAttack, EnemyAction::PowerAttackRecover}) {
        sense.action = action;
        intent = lunge.think(brood, sense);
        CHECK(intent.pace == 0);
        CHECK(brood.counter == 37);
    }
    sense.action = EnemyAction::Start;
    CHECK(lunge.think(brood, sense).pace == 0);
    CHECK(brood.counter == 37);
}

TEST_CASE("wanderers probe ahead and mirror quarter turns through a twenty-tick corner hold",
          "[game][enemies][mind][wander-parity]") {
    // move_logic05/06 (0x80047844/0x80047BF0) probe before a collision is reported.
    for (const s32 way : {kWanderWay, kWanderOtherWay}) {
        const f32 side = way == kWanderWay ? -1.0f : 1.0f;
        MindMemory memory;
        MindSense sense;
        sense.ticks = 2;
        sense.wanderClear = [](f32) { return false; };
        const EnemyMind& wander = enemyMindOf(way);
        CHECK(wander.think(memory, sense).heading == Approx(side * kPi / 2));
        CHECK(memory.deadEnd == 20);
        CHECK(memory.turns == 0);
        sense.wanderClear = [](f32) { return true; };
        for (s32 step = 0; step < 9; ++step) {
            CHECK(wander.think(memory, sense).heading == Approx(side * kPi / 2));
        }
        CHECK(wander.think(memory, sense).heading == Approx(kPi));
        CHECK(memory.deadEnd == 0);
        CHECK(memory.turns == 1);
        // A fresh failed probe turns immediately, even with a hold already running.
        sense.wanderClear = [](f32) { return false; };
        wander.think(memory, sense);
        CHECK(memory.deadEnd == 20);
        CHECK(wander.think(memory, sense).heading == Approx(0).margin(0.000001));
        CHECK(memory.deadEnd == 18);
    }
}

TEST_CASE("unseen seeking and chasing alternate their fallback wander direction by slot",
          "[game][enemies][mind][wander-parity]") {
    for (const s32 way : {kSeekWay, kChaseWay, kZigZagWay, kCastWay}) {
        for (const bool mirrored : {false, true}) {
            MindSense sense = senseAhead();
            sense.recognized = false;
            sense.mirroredWander = mirrored;
            sense.wanderClear = [](f32) { return false; };
            MindMemory memory;
            const auto intent = enemyMindOf(way).think(memory, sense);
            CHECK(intent.heading == Approx((mirrored ? 1.0f : -1.0f) * kPi / 2));
            CHECK(intent.pace == 1);
        }
    }
}

TEST_CASE("a sense tells the way to its player and which side round is nearer",
          "[game][enemies][mind]") {
    MindSense sense = senseAhead();
    sense.targetPosition = Vec3{-10.0f, 0.0f, 0.0f};
    REQUIRE(sense.faceAngle(1.0f) == Approx(-kPi / 2.0f));
    sense.target = -1;
    REQUIRE(sense.faceAngle(1.0f) == 1.0f);
    REQUIRE(sense.nearerSide() == 1);
    // Facing +z with the player off to the left (-x): the left probe ends nearer, so the
    // route is the left one, negative; off to the right, the right one.
    sense = senseAhead();
    sense.targetPosition = Vec3{-10.0f, 0.0f, 5.0f};
    REQUIRE(sense.nearerSide() == -1);
    sense.targetPosition = Vec3{10.0f, 0.0f, 5.0f};
    REQUIRE(sense.nearerSide() == 1);
    REQUIRE(sense.clearAlong(0.0f)); // nothing to probe with: all clear
}

} // namespace
