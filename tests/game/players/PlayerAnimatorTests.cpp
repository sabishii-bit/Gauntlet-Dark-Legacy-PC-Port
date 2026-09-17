#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"

#include "game/players/PlayerAnimator.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Action = PlayerAnimator::Action;

constexpr s32 kTicks = 2; ///< per frame at thirty frames a second
constexpr f32 kStep = 1.0f / 30.0f;

/** The class sequences with their real lengths, each sliding the one node along x by its
 * own index so the pose tells which is playing. */
TreeInfo classTree() {
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo root;
    root.name = "L1ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    struct Entry {
        const char* name;
        s32 frames;
        s32 rate;
        bool repeats;
    };
    const std::array<Entry, 9> entries{{{"READY", 60, 30, true},
                                        {"IDLE1", 150, 45, false},
                                        {"IDLE2", 71, 30, false},
                                        {"IDLE2_LOOP", 69, 30, true},
                                        {"START", 60, 30, false},
                                        {"WALK1", 12, 30, false},
                                        {"WALK2", 11, 30, false},
                                        {"RUN1", 10, 30, false},
                                        {"RUN2", 10, 30, false}}};
    u32 index = 0;
    for (const Entry& entry : entries) {
        TreeSequenceInfo sequence;
        sequence.name = entry.name;
        sequence.frames = entry.frames;
        sequence.frameRate = entry.rate;
        sequence.repeats = entry.repeats;
        TrackInfo track;
        track.node = 0;
        track.flags = TrackInfo::channelBit(3);
        track.frames = {0};
        track.values = {static_cast<f32>(index)};
        sequence.tracks.push_back(track);
        sequence.trackOfNode = {0};
        tree.sequences.push_back(sequence);
        ++index;
    }
    return tree;
}

f32 playingIndex(const PlayerAnimator& animator) {
    return animator.pose().matrices()[0][3].x;
}

int stepsUntil(PlayerAnimator& animator, PlayerMotion motion, Action wanted, int limit) {
    int steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.update(motion, kTicks, kStep);
        ++steps;
    }
    return steps;
}

TEST_CASE("the stick's magnitude picks standing, walking or running", "[game][players][animation]") {
    REQUIRE(PlayerAnimator::motionFor(0.0f) == PlayerMotion::Stand);
    REQUIRE(PlayerAnimator::motionFor(0.3f) == PlayerMotion::Walk);
    REQUIRE(PlayerAnimator::motionFor(0.75f) == PlayerMotion::Walk);
    REQUIRE(PlayerAnimator::motionFor(0.76f) == PlayerMotion::Run);
}

TEST_CASE("a character plays its entrance, settles into its stance and walks in two halves",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE_FALSE(animator.bound());
    REQUIRE(animator.bind(tree));
    REQUIRE(animator.bound());
    REQUIRE(animator.action() == Action::Ready);
    REQUIRE(animator.sequenceOf(Action::Start) == 4);

    // The entrance cuts in at once and, asked to walk, plays out first.
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    REQUIRE(animator.action() == Action::Start);
    REQUIRE(playingIndex(animator) == 4.0f);
    const int untilWalk = stepsUntil(animator, PlayerMotion::Walk, Action::Walk1, 200);
    REQUIRE(untilWalk == 60);
    REQUIRE(playingIndex(animator) == 5.0f);

    // The walk's halves take turns as each cycle ends, each end a foot coming down.
    REQUIRE(animator.footfall() == PlayerAnimator::Foot::None);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::Walk2, 50) == 12);
    REQUIRE(animator.footfall() == PlayerAnimator::Foot::First);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::Walk1, 50) == 11);
    REQUIRE(animator.footfall() == PlayerAnimator::Foot::Second);
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    REQUIRE(animator.footfall() == PlayerAnimator::Foot::None);

    // Letting go finishes the half cycle (a frame in already), then eases back into the
    // stance over two ticks.
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(animator.action() == Action::Walk1);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 50) == 10);
    REQUIRE(animator.footfall() == PlayerAnimator::Foot::First); // the stride ends on a step
    REQUIRE(animator.player().transitioning());
    REQUIRE(playingIndex(animator) == Approx(5.0f)); // the blend starts from the walk
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(playingIndex(animator) == Approx(2.5f));
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE_FALSE(animator.player().transitioning());
    REQUIRE(playingIndex(animator) == 0.0f);

    // Running alternates the same way and pushing the stick cuts the stance short at once.
    animator.update(PlayerMotion::Run, kTicks, kStep);
    REQUIRE(animator.action() == Action::Run1);
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run2, 50) == 10);
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 50) == 10);
}

TEST_CASE("standing still long enough brings the fidgets and moving ends them",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(animator.action() == Action::Ready);
    // A minute of standing (1800 ticks), then the fidget waits for the stance loop to end.
    for (int i = 0; i < 900; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
    }
    REQUIRE(animator.action() == Action::Ready);
    REQUIRE(animator.stillTicks() > PlayerAnimator::kFidgetTicks);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Idle1, 61) <= 60);
    // The fidget (150 frames at twenty a second) plays out, then the stance returns and the
    // second timer starts.
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 300) == 225);
    REQUIRE(animator.stillTicks() == 0);
    REQUIRE(animator.fidgetTicks() >= 1);
    // Twenty seconds later the second fidget comes, and loops.
    for (int i = 0; i < 300; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
    }
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Idle2, 61) <= 60);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Idle2Loop, 100) == 71);
    for (int i = 0; i < 200; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
    }
    REQUIRE(animator.action() == Action::Idle2Loop);
    // Moving cuts the fidget short and clears the timers.
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    REQUIRE(animator.action() == Action::Walk1);
    REQUIRE(animator.stillTicks() == 0);
    REQUIRE(animator.fidgetTicks() == 0);

    // Without a stance the tree cannot be bound; a missing half falls back to the stance.
    TreeInfo bare = classTree();
    bare.sequences[0].name = "OTHER";
    REQUIRE_FALSE(animator.bind(bare));
    REQUIRE_FALSE(animator.bound());
    TreeInfo noHalf = classTree();
    noHalf.sequences[6].name = "OTHER";
    REQUIRE(animator.bind(noHalf));
    REQUIRE(animator.sequenceOf(Action::Walk2) == 0);
}

} // namespace
