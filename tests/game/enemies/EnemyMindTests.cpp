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

TEST_CASE("the seek tries a sixteenth either side of straight, the prowler pounces within eight, "
          "and the wanderer turns at dead ends",
          "[game][enemies][mind]") {
    const EnemyMind& seek = enemyMindOf(kSeekWay);
    MindMemory memory;
    MindSense sense = senseAhead();
    // Straight is blocked, right is clear: a sixteenth right is taken.
    sense.clear = [](f32 heading) { return heading > 0.1f; };
    MindIntent intent = seek.think(memory, sense);
    REQUIRE(intent.heading == Approx(kPi / 8.0f));
    // Only the left is clear, and only well round.
    sense.clear = [](f32 heading) { return heading < -1.0f; };
    intent = seek.think(memory, sense);
    REQUIRE(intent.heading == Approx(-3.0f * kPi / 8.0f));
    // Nothing clear: straight anyway.
    sense.clear = [](f32) { return false; };
    intent = seek.think(memory, sense);
    REQUIRE(intent.heading == 0.0f);
    // The prowler wanders until a player is within eight, then seeks for good.
    const EnemyMind& prowl = enemyMindOf(kProwlWay);
    MindMemory rat;
    rat.heading = 1.0f;
    intent = prowl.think(rat, senseAhead(20.0f));
    REQUIRE(intent.heading == 1.0f);
    REQUIRE_FALSE(intent.become.has_value());
    intent = prowl.think(rat, senseAhead(7.0f));
    REQUIRE(intent.heading == 0.0f);
    REQUIRE(intent.become == kSeekWay);
    // The wanderer keeps straight on, turns an eighth at a bump and holds it thirty ticks,
    // faces a player against it.
    const EnemyMind& wander = enemyMindOf(kWanderWay);
    MindMemory roaming;
    roaming.heading = 0.0f;
    MindSense bump;
    bump.ticks = 2;
    bump.bumpedWall = true;
    intent = wander.think(roaming, bump);
    REQUIRE(intent.heading == Approx(kPi / 4.0f));
    REQUIRE(roaming.turns == 1);
    REQUIRE(roaming.deadEnd == 30);
    intent = wander.think(roaming, bump);
    REQUIRE(intent.heading == Approx(kPi / 4.0f)); // held
    REQUIRE(roaming.turns == 1);
    MindSense touching;
    touching.contact = 0;
    touching.contactPosition = Vec3{-5.0f, 0.0f, 0.0f};
    intent = wander.think(roaming, touching);
    REQUIRE(intent.heading == Approx(-kPi / 2.0f));
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
    CHECK(intent.heading == Approx(-kPi / 2.0f));
    CHECK(intent.pace == 1.0f);
    CHECK(memory.zigZag.count == 43);
    // Every 45 ticks another. Four bring it back to straight at the player; once it has made
    // four and drifted over a quarter turn off (the sixth), it aims afresh an eighth of a
    // turn off straight at them, and holds that aim thirty ticks.
    s32 ticks = 2;
    for (; ticks < 600 && memory.zigZag.hold != 30; ticks += 2) {
        zig.think(memory, sense);
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
    MindSense sense = senseAhead(20.0f);
    sense.tier = 3;
    sense.castWait = 90;
    sense.random = 3; // a window of 23 ticks, and a wait of 23 + 90
    // The first thing it does on seeing its player is cast, the power attack at strength
    // three, while it goes on towards them.
    MindIntent intent = cast.think(memory, sense);
    REQUIRE(intent.action == EnemyAction::PowerAttack);
    REQUIRE(intent.heading == Approx(0.0f));
    REQUIRE(memory.deadEnd == 21); // counted down as it seeks, this tick too
    REQUIRE(memory.fuse == 23 + 90);
    for (s32 tick = 2; tick < 23; tick += 2) {
        REQUIRE(cast.think(memory, sense).action == EnemyAction::PowerAttack);
    }
    // The window out, it walks until its wait is.
    intent = cast.think(memory, sense);
    REQUIRE(intent.action == EnemyAction::Walk);
    MindMemory weak;
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
    REQUIRE(intent.pace == 0.5f);
    MindMemory far;
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
