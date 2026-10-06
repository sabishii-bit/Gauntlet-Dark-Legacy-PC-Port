#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyMind.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Action = EnemyAction;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

/** A grunt's sequences: no DEATH, no ATTACK2, a ready-to-walk in and out. */
TreeInfo gruntTree() {
    TreeInfo tree;
    tree.name = "GRU1";
    TreeNodeInfo root;
    root.name = "OANIM";
    root.type = 2;
    tree.nodes.push_back(root);
    struct Entry {
        const char* name;
        s32 frames;
        bool repeats;
    };
    const std::array<Entry, 12> entries{{{"READY", 20, true},
                                         {"WALK", 16, true},
                                         {"ATTACK1", 8, false},
                                         {"ATTACK1R", 8, false},
                                         {"ATTACK3", 10, false},
                                         {"ATTACK3R", 10, false},
                                         {"START", 12, false},
                                         {"HIT1", 6, false},
                                         {"HIT2", 12, false},
                                         {"GETUP", 10, false},
                                         {"READYTOWALK", 4, false},
                                         {"WALKTOREADY", 4, false}}};
    for (const Entry& entry : entries) {
        TreeSequenceInfo sequence;
        sequence.name = entry.name;
        sequence.frames = entry.frames;
        sequence.frameRate = 30;
        sequence.repeats = entry.repeats;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

s32 stepsUntil(EnemyAnimator& animator, Action ask, Action wanted, s32 limit) {
    s32 steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.request(ask);
        animator.update(kTicks, kStep);
        ++steps;
    }
    return steps;
}

TEST_CASE("a collision hold cancels pending walk and run without interrupting other requests",
          "[enemies][animation][alpha-seek-blocked]") {
    // fn_8004D030 assigns daction=READY only for WALK/RUN. A low-priority
    // request(READY) cannot replace those, and must not cancel an attack or hit.
    const auto tree = gruntTree();
    for (usize i = 0; i < kEnemyActionCount; ++i) {
        EnemyAnimator animator;
        REQUIRE(animator.bind(tree));
        const auto action = static_cast<Action>(i);
        CAPTURE(i);
        animator.request(action);
        animator.stopWalking();
        CHECK(animator.requested() ==
              (action == Action::Walk || action == Action::Run ? Action::Ready : action));
        CHECK(animator.action() == Action::Start); // no animation step or restart
    }
}

TEST_CASE("enemy presentation interpolates without changing attack events and holds across cuts",
          "[enemies][animation][presentation]") {
    TreeInfo tree = gruntTree();
    for (auto& sequence : tree.sequences) {
        TrackInfo track;
        track.node = 0;
        track.flags = TrackInfo::channelBit(3);
        track.frames = {0, 60};
        track.values = {0, 60};
        sequence.tracks = {track};
        sequence.trackOfNode = {0};
    }
    EnemyAnimator native;
    EnemyAnimator shown;
    REQUIRE(native.bind(tree));
    REQUIRE(shown.bind(tree));
    TreePose pose;
    native.update(kTicks, kStep);
    shown.update(kTicks, kStep);
    for (const f32 fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        shown.evaluatePresentation(pose, fraction);
        CHECK(pose.matrices()[0][3].x == Catch::Approx(fraction));
        CHECK(shown.presentationFrame(fraction) == Catch::Approx(fraction));
    }
    s32 blows = 0;
    for (s32 step = 0; step < 150; ++step) {
        native.request(Action::Attack);
        shown.request(Action::Attack);
        native.update(kTicks, kStep);
        shown.update(kTicks, kStep);
        for (const f32 fraction : {0.0f, 0.5f, 1.0f}) {
            shown.evaluatePresentation(pose, fraction);
        }
        CHECK(shown.player().frame() == native.player().frame());
        CHECK(shown.pose().matrices()[0] == native.pose().matrices()[0]);
        CHECK(shown.struck() == native.struck());
        CHECK(shown.threw() == native.threw());
        blows += shown.struck() ? 1 : 0;
    }
    CHECK(blows > 0);
    shown.holdPresentation();
    shown.evaluatePresentation(pose, 0);
    const Mat4 held = pose.matrices()[0];
    shown.evaluatePresentation(pose, 1);
    CHECK(pose.matrices()[0] == held);
    shown.request(Action::Dying);
    shown.update(kTicks, kStep);
    shown.evaluatePresentation(pose, 0);
    const Mat4 cut = pose.matrices()[0];
    shown.evaluatePresentation(pose, 1);
    CHECK(pose.matrices()[0] == cut);
}

TEST_CASE("an enemy walks in, is asked by priority, and lands its blow as the swing ends",
          "[game][enemies][animation]") {
    const TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, true));
    REQUIRE(animator.entering());
    REQUIRE(animator.has(Action::Attack));
    REQUIRE_FALSE(animator.has(Action::Dying));
    REQUIRE_FALSE(animator.has(Action::Attack2));
    // The entrance plays out whatever is asked, then a grunt goes straight to walking.
    REQUIRE(stepsUntil(animator, Action::Walk, Action::Walk, 30) < 30);
    REQUIRE(animator.moving());
    // Left alone it comes to rest by way of the walk-to-ready.
    REQUIRE(stepsUntil(animator, Action::Ready, Action::WalkToReady, 30) < 30);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    // A louder request wins the tick; a quieter one is refused.
    animator.request(Action::Walk);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Attack);
    animator.request(Action::Walk);
    REQUIRE(animator.requested() == Action::Attack);
    animator.request(Action::HitReact1);
    REQUIRE(animator.requested() == Action::HitReact1);
    animator.update(kTicks, kStep);
    REQUIRE(animator.action() == Action::HitReact1);
    REQUIRE(animator.reacting());
    REQUIRE(animator.requested() == Action::Ready); // forgotten with the tick
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    // An attack asked from the stance waits for the stance's loop to end, then swings; the
    // blow lands as the swing gives way to its recovery, once.
    REQUIRE(stepsUntil(animator, Action::Attack, Action::Attack, 60) < 60);
    REQUIRE(animator.swinging());
    REQUIRE(animator.attacking());
    s32 struck = 0;
    for (s32 i = 0; i < 40; ++i) {
        animator.request(Action::Ready);
        animator.update(kTicks, kStep);
        struck += animator.struck() ? 1 : 0;
        if (animator.struck()) {
            REQUIRE(animator.action() == Action::AttackRecover);
        }
    }
    REQUIRE(struck == 1);
    REQUIRE(animator.action() == Action::Ready);
    // The power blow is told apart.
    REQUIRE(stepsUntil(animator, Action::PowerAttack, Action::PowerAttack, 60) < 60);
    s32 power = 0;
    for (s32 i = 0; i < 40; ++i) {
        animator.update(kTicks, kStep);
        power += animator.powerStruck() ? 1 : 0;
        REQUIRE_FALSE(animator.struck());
    }
    REQUIRE(power == 1);
    // A hit cuts into a swing at once; the knock-down gets up again.
    REQUIRE(stepsUntil(animator, Action::Attack, Action::Attack, 60) < 60);
    animator.request(Action::HitReact2);
    animator.update(kTicks, kStep);
    REQUIRE(animator.action() == Action::HitReact2);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::GetUp, 40) < 40);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 40) < 40);
    // A death the tree lacks plays as the knock-down, and is done when that is.
    animator.request(Action::Dying);
    animator.update(kTicks, kStep);
    REQUIRE(animator.dying());
    REQUIRE_FALSE(animator.dead());
    REQUIRE(animator.player().sequence() == animator.sequenceOf(Action::HitReact2));
    s32 until = 0;
    while (!animator.dead() && until < 60) {
        animator.request(Action::Dying);
        animator.update(kTicks, kStep);
        ++until;
    }
    REQUIRE(animator.dead());
    REQUIRE(until >= 5);
}

TEST_CASE("a walk pending as the tick's default refuses the stance and, as loud, a run",
          "[game][enemies][animation]") {
    // The scorpion's default action is the walk (do_enemies), which RequestEnemyAction's
    // priorities then hold against the stance and a run, though not against an attack.
    const TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, true));
    animator.request(Action::Walk);
    animator.request(Action::Ready);
    CHECK(animator.requested() == Action::Walk);
    animator.request(Action::Run);
    CHECK(animator.requested() == Action::Walk);
    animator.request(Action::Attack);
    CHECK(animator.requested() == Action::Attack);
}

TEST_CASE("a body idling after a throw refuses to attack, and a tree without a stance is refused",
          "[game][enemies][animation]") {
    TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setIdle(1.0f);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Ready);
    animator.request(Action::Walk);
    REQUIRE(animator.requested() == Action::Walk);
    for (s32 i = 0; i < 40; ++i) {
        animator.update(kTicks, kStep);
    }
    REQUIRE(animator.idleSeconds() == 0.0f);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Attack);
    animator.unbind();
    REQUIRE_FALSE(animator.bound());
    tree.sequences.erase(tree.sequences.begin());
    REQUIRE_FALSE(animator.bind(tree));
}

TEST_CASE("repeated death requests finish once and never stand back up",
          "[game][enemies][animation][enemy-feedback]") {
    for (const bool deathSequence : {false, true}) {
        TreeInfo tree = gruntTree();
        if (deathSequence) {
            TreeSequenceInfo sequence = tree.sequences[8];
            sequence.name = "DEATH";
            tree.sequences.push_back(sequence);
        }
        EnemyAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.request(Action::HitReact2);
        animator.update(kTicks, kStep);
        REQUIRE(animator.action() == Action::HitReact2);
        animator.request(Action::Dying);
        animator.update(kTicks, kStep);
        REQUIRE(animator.action() == Action::Dying);
        for (s32 frame = 0; frame < 30; ++frame) {
            animator.request(Action::Dying);
            animator.update(kTicks, kStep);
            CHECK(animator.action() == Action::Dying);
            CHECK_FALSE(animator.struck());
        }
        CHECK(animator.dead());
    }
}

TEST_CASE("throw stages carry fractional retail waits without blocking movement or reactions",
          "[game][enemies][animation][battle-archer]") {
    TreeInfo tree = gruntTree();
    for (const auto* name : {"THROW1", "THROW2", "THROWF", "ATTTOREADY"}) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = 3;
        sequence.repeats = false;
        tree.sequences.push_back(sequence);
    }
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree));
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    SECTION("an exact second stays in the carry until the next throw stage") {
        animator.setThrowInterval(1.0f);
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
        CHECK(animator.idleSeconds() == 0.0f);
        REQUIRE(stepsUntil(animator, Action::Throw, Action::ThrowFinish, 30) < 30);
        CHECK(animator.threw());
        CHECK(animator.idleSeconds() == Catch::Approx(1.1f));
        animator.request(Action::Throw);
        CHECK(animator.requested() == Action::Ready);
        animator.request(Action::RunAttack);
        CHECK(animator.requested() == Action::RunAttack);
        animator.request(Action::HitReact1);
        animator.update(kTicks, kStep);
        CHECK(animator.action() == Action::HitReact1);
        REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 60) < 60);
        for (s32 frame = 0; frame < 40; ++frame) {
            animator.update(kTicks, kStep);
        }
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
    }
    SECTION("quarter seconds accumulate at stage changes, not every animation tick") {
        animator.setThrowInterval(0.25f);
        for (const Action next :
             {Action::Throw, Action::ThrowFinish, Action::Throw2, Action::ThrowFinish}) {
            REQUIRE(stepsUntil(animator, Action::Throw, next, 30) < 30);
            CHECK(animator.idleSeconds() == 0.0f);
        }
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw2, 30) < 30);
        CHECK(animator.idleSeconds() == Catch::Approx(1.1f));
    }
    SECTION("the placement's no-wait interval never adds a pause") {
        animator.setThrowInterval(0.0f);
        for (s32 frame = 0; frame < 90; ++frame) {
            animator.request(Action::Throw);
            animator.update(kTicks, kStep);
            CHECK(animator.idleSeconds() == 0.0f);
        }
    }
    // Reusing an animator must not inherit either the interval or its fractional carry.
    REQUIRE(animator.bind(tree));
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
    CHECK(animator.idleSeconds() == 0.0f);
}

TEST_CASE("the Province suicide bomber stays still throughout its authored running windup",
          "[game][enemies][animation][alpha-bomber-start][assets]") {
    const auto path = test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    AnimationSet actions;
    REQUIRE(actions.load(path.parent_path()));
    const auto found = actions.find("GRUS");
    REQUIRE(found);
    const auto& tree = actions.tree(*found);
    const auto windup = tree.findSequence("READYTOWALK");
    REQUIRE(windup);
    REQUIRE(tree.findSequence("RUN"));
    // The authored bomber has no WALK, so action 9 finishes into RUN (4).
    REQUIRE_FALSE(tree.findSequence("WALK"));
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree));
    MindMemory memory;
    MindSense sense;
    sense.target = 0;
    sense.targetPosition = {0, 0, 12};
    sense.targetDistance = 12;
    sense.recognized = true;
    sense.ticks = kTicks;
    const auto& mind = enemyMindOf(kSuicideWay);
    bool sawWindup = false;
    s32 windupUpdates = 0;
    for (s32 tick = 0; tick < 180 && animator.action() != Action::Run; ++tick) {
        sense.action = animator.action();
        const auto intent = mind.think(memory, sense);
        CHECK(intent.pace == 0);
        CHECK_FALSE(intent.yell);
        if (sense.action == Action::ReadyToWalk) {
            sawWindup = true;
            ++windupUpdates;
            CHECK(animator.player().sequence() == *windup);
        }
        animator.request(intent.action);
        animator.update(kTicks, kStep);
    }
    REQUIRE(sawWindup);
    REQUIRE(windupUpdates > 1);
    REQUIRE(animator.action() == Action::Run);
    sense.action = animator.action();
    const auto run = mind.think(memory, sense);
    CHECK(run.yell);
    CHECK(run.pace == 1.5f);
    CHECK(run.action == Action::Run);
    CHECK(memory.counter == kTicks);
}

} // namespace
