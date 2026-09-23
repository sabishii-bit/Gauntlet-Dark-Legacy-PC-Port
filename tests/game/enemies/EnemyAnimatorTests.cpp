#include <array>
#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"

#include "game/enemies/EnemyAnimator.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Action = EnemyAction;

constexpr std::int32_t kTicks = 2;
constexpr float kStep = 1.0f / 30.0f;

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
        std::int32_t frames;
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

int stepsUntil(EnemyAnimator& animator, Action ask, Action wanted, int limit) {
    int steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.request(ask);
        animator.update(kTicks, kStep);
        ++steps;
    }
    return steps;
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
    int struck = 0;
    for (int i = 0; i < 40; ++i) {
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
    int power = 0;
    for (int i = 0; i < 40; ++i) {
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
    int until = 0;
    while (!animator.dead() && until < 60) {
        animator.request(Action::Dying);
        animator.update(kTicks, kStep);
        ++until;
    }
    REQUIRE(animator.dead());
    REQUIRE(until >= 5);
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
    for (int i = 0; i < 40; ++i) {
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

} // namespace
