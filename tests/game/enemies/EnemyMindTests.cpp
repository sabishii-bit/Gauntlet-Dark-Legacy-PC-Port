#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(enemyMindOf(kStandWay).name() == "stand");
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
    for (int i = 0; i < 5; ++i) {
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
    for (int b = 0; b < 7; ++b) {
        chase.think(memory, sense);
        for (int i = 0; i < 6; ++i) {
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
    for (int i = 0; i < 10; ++i) {
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
    REQUIRE(intent.heading == Approx(2.0f * kPi / 64.0f));
    REQUIRE_FALSE(intent.expire);
    sense.generatorGone = true;
    REQUIRE(loiter.think(memory, sense).expire);
    // The fleer runs straight away from its player, or round whatever is in the way.
    const EnemyMind& flee = enemyMindOf(kFleeWay);
    MindMemory fleeing;
    intent = flee.think(fleeing, senseAhead());
    REQUIRE(std::abs(intent.heading) == Approx(kPi));
    REQUIRE(intent.pace > 1.0f);
    REQUIRE(intent.action == EnemyAction::Run);
    MindSense cornered = senseAhead();
    cornered.clear = [](f32 heading) { return std::abs(heading) < 2.0f; };
    intent = flee.think(fleeing, cornered);
    REQUIRE(std::abs(intent.heading) < 2.0f);
    MindSense alone;
    alone.ticks = 2;
    fleeing.heading = 0.7f;
    intent = flee.think(fleeing, alone);
    REQUIRE(intent.heading == 0.7f); // nobody to flee: wandering
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
    // The stander only faces whoever comes against it.
    const EnemyMind& stand = enemyMindOf(kStandWay);
    MindMemory standing;
    MindSense touching;
    touching.contact = 0;
    touching.contactPosition = Vec3{5.0f, 0.0f, 0.0f};
    intent = stand.think(standing, touching);
    REQUIRE(intent.pace == 0.0f);
    REQUIRE(intent.heading == Approx(kPi / 2.0f));
    REQUIRE(intent.action == EnemyAction::Ready);
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
