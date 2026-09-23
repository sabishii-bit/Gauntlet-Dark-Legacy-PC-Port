#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"

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
    const std::array<Entry, 23> entries{
        {{"READY", 60, 30, true},        {"IDLE1", 150, 45, false},
         {"IDLE2", 71, 30, false},       {"IDLE2_LOOP", 69, 30, true},
         {"START", 60, 30, false},       {"WALK1", 12, 30, false},
         {"WALK2", 11, 30, false},       {"RUN1", 10, 30, false},
         {"RUN2", 10, 30, false},        {"THROW1S", 10, 24, false},
         {"THROW1", 3, 24, false},       {"THROW1R", 10, 24, false},
         {"THROW2S", 10, 24, false},     {"THROW2", 2, 24, false},
         {"THROW2R", 10, 24, false},     {"MAGICS", 11, 30, false},
         {"MAGICR", 15, 30, false},      {"THROWPOTIONS", 11, 30, false},
         {"THROWPOTIONR", 9, 30, false}, {"DEATH", 20, 30, false},
         {"HITREACT", 11, 30, false},    {"STUN1", 15, 30, false},
         {"SPIKEHIT", 15, 30, false}}};
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

s32 stepsUntil(PlayerAnimator& animator, PlayerMotion motion, Action wanted, s32 limit) {
    s32 steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.update(motion, kTicks, kStep);
        ++steps;
    }
    return steps;
}

s32 stepsUntilAttack(PlayerAnimator& animator, Action wanted, s32 limit) {
    s32 steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, true);
        ++steps;
    }
    return steps;
}

TEST_CASE("spike hits interrupt an attack with their own animation then release control",
          "[game][players][animation][player-impact]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::Throw);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Spike);
    REQUIRE(animator.action() == Action::SpikeHit);
    REQUIRE(playingIndex(animator) == 22);
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == 0);
    REQUIRE_FALSE(animator.released());
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::SpikeHit);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);
    REQUIRE_FALSE(animator.reacting());
}

TEST_CASE("a held attack winds up, lets go and recovers, over and over",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    REQUIRE_FALSE(animator.throwing());
    REQUIRE(animator.moveScale() == 1.0f);
    // The attack cuts into the stance at once.
    animator.update(PlayerMotion::Stand, kTicks, kStep, true);
    REQUIRE(animator.action() == Action::Throw);
    REQUIRE(animator.throwing());
    REQUIRE(animator.moveScale() == 0.0f);
    REQUIRE_FALSE(animator.released());
    // From its second frame (at twenty-four a second) the wind-up gives way to the release.
    s32 steps = 0;
    while (animator.action() == Action::Throw && steps < 20) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, true);
        ++steps;
    }
    REQUIRE(animator.action() == Action::ThrowRelease);
    REQUIRE(steps >= 3);
    REQUIRE(steps <= 4);
    // The release runs out its three frames; its end is the moment the weapon flies, once.
    s32 releases = 0;
    steps = 0;
    while (animator.action() == Action::ThrowRelease && steps < 20) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, true);
        releases += animator.released() ? 1 : 0;
        ++steps;
    }
    REQUIRE(animator.action() == Action::ThrowRecover);
    REQUIRE(releases == 1);
    REQUIRE(animator.recovering());
    REQUIRE(animator.attackSeconds() > 0.15f);
    REQUIRE(animator.attackSeconds() < 0.4f);
    animator.update(PlayerMotion::Stand, kTicks, kStep, true);
    REQUIRE_FALSE(animator.released());
    // Still held, the recovery leads into the next throw; the stick moves nothing meanwhile.
    REQUIRE(stepsUntilAttack(animator, Action::Throw, 40) > 5);
    REQUIRE_FALSE(animator.recovering());
    // Let go mid-throw, the throw still plays out, then the body eases back to its stance.
    while (animator.throwing() && steps < 200) {
        animator.update(PlayerMotion::Run, kTicks, kStep, false);
        REQUIRE((animator.action() == Action::Ready || animator.throwing() ||
                 animator.action() == Action::Run1));
        ++steps;
    }
    REQUIRE_FALSE(animator.throwing());
    REQUIRE(animator.moveScale() == 1.0f);
}

TEST_CASE("a potion is raised, then released once, however long its button is held",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::UsePotion);
    REQUIRE(animator.action() == Action::UsePotion);
    REQUIRE(animator.conjuring());
    REQUIRE(animator.moveScale() == 0.0f);
    s32 used = 0;
    s32 steps = 0;
    while (animator.conjuring() && steps < 120) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::UsePotion);
        used += animator.potionUsed() ? 1 : 0;
        REQUIRE_FALSE(animator.potionThrown());
        ++steps;
    }
    REQUIRE(used == 1);
    REQUIRE(steps > 20); // eleven frames up and fifteen down
    REQUIRE(animator.action() == Action::Ready);
    // Thrown, it is the other pair of sequences and the other moment.
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
    REQUIRE(animator.action() == Action::ThrowPotion);
    s32 thrown = 0;
    steps = 0;
    while (animator.conjuring() && steps < 120) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
        thrown += animator.potionThrown() ? 1 : 0;
        ++steps;
    }
    REQUIRE(thrown == 1);
    REQUIRE_FALSE(animator.throwing());
}

TEST_CASE("an attack from the first half of a walk or run takes the moving wind-up",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Run, kTicks, kStep);
    REQUIRE(animator.action() == Action::Run1);
    animator.update(PlayerMotion::Run, kTicks, kStep, true);
    REQUIRE(animator.action() == Action::ThrowMoving);
    s32 steps = 0;
    while (animator.action() != Action::ThrowMovingRecover && steps < 40) {
        animator.update(PlayerMotion::Run, kTicks, kStep, true);
        ++steps;
    }
    REQUIRE(animator.action() == Action::ThrowMovingRecover);
    // A class without the throw's sequences does not throw.
    TreeInfo bare = classTree();
    std::erase_if(bare.sequences,
                  [](const TreeSequenceInfo& s) { return s.name.starts_with("THROW"); });
    PlayerAnimator plain;
    REQUIRE(plain.bind(bare, false));
    plain.update(PlayerMotion::Stand, kTicks, kStep, true);
    REQUIRE(plain.action() == Action::Ready);
}

TEST_CASE("the stick's magnitude picks standing, walking or running",
          "[game][players][animation]") {
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
    const s32 untilWalk = stepsUntil(animator, PlayerMotion::Walk, Action::Walk1, 200);
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
    for (s32 i = 0; i < 900; ++i) {
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
    for (s32 i = 0; i < 300; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
    }
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Idle2, 61) <= 60);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Idle2Loop, 100) == 71);
    for (s32 i = 0; i < 200; ++i) {
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

TEST_CASE("a character that dies falls once and stays down, whatever is asked of it",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    // Even out of a throw about to leave the hand, which then never flies.
    REQUIRE(stepsUntilAttack(animator, Action::ThrowRelease, 200) < 200);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Die);
    REQUIRE(animator.action() == Action::Death);
    REQUIRE(animator.dying());
    REQUIRE_FALSE(animator.dead());
    REQUIRE_FALSE(animator.released());
    s32 steps = 0;
    while (!animator.dead() && steps < 400) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
        REQUIRE(animator.action() == Action::Death);
        REQUIRE_FALSE(animator.released());
        ++steps;
    }
    REQUIRE(animator.dead());
    REQUIRE(steps >= 15); // twenty frames at the sequence's pace, not at once
    animator.update(PlayerMotion::Run, kTicks, kStep);
    REQUIRE(animator.action() == Action::Death);
    // Bound again, it stands.
    REQUIRE(animator.bind(tree, false));
    REQUIRE_FALSE(animator.dead());
    REQUIRE(animator.action() == Action::Ready);
}

TEST_CASE("struck, a character flinches or reels where it stands and then carries on",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 10) < 10);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Flinch);
    REQUIRE(animator.action() == Action::HitReact);
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == 0.0f);
    // Struck again meanwhile it is not set reeling anew, and nothing else is heeded.
    const f32 frame = animator.player().frame();
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Reel);
    REQUIRE(animator.action() == Action::HitReact);
    REQUIRE(animator.player().frame() > frame);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::HitReact);
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 60) < 60);
    REQUIRE_FALSE(animator.reacting());
    REQUIRE(animator.moveScale() == 1.0f);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Reel);
    REQUIRE(animator.action() == Action::Stun);
    // A throw it cuts into never leaves the hand.
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);
    REQUIRE(stepsUntilAttack(animator, Action::ThrowRelease, 200) < 200);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Flinch);
    REQUIRE(animator.action() == Action::HitReact);
    REQUIRE_FALSE(animator.released());
}

TEST_CASE("a turbo move cuts in, plays through unheeding, and is known as it begins",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        tree.sequences.push_back(sequence);
    };
    add("ATTPWRB", 12);
    add("ATTPWRC", 12);
    add("SHOVE", 8);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    REQUIRE_FALSE(animator.canBegin(PlayerDeed::Attack));
    REQUIRE(animator.canBegin(PlayerDeed::TurboFull));
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 10) < 10);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::TurboStrong);
    REQUIRE(animator.action() == Action::TurboStrong);
    REQUIRE(animator.turboBegan());
    REQUIRE(animator.turboing());
    REQUIRE(animator.moveScale() == 0.0f);
    REQUIRE_FALSE(animator.canBegin(PlayerDeed::Shove)); // one at a time
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::TurboFull);
    REQUIRE(animator.action() == Action::TurboStrong);
    REQUIRE_FALSE(animator.turboBegan()); // only the tick it began
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::TurboStrong);
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 60) < 60);
    REQUIRE_FALSE(animator.turboing());
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Shove);
    REQUIRE(animator.action() == Action::Shove);
    // A hit cuts even into that.
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Flinch);
    REQUIRE(animator.action() == Action::HitReact);
    // A class without the sequence does not do the move.
    const TreeInfo plain = classTree();
    PlayerAnimator other;
    REQUIRE(other.bind(plain, false));
    REQUIRE_FALSE(other.canBegin(PlayerDeed::TurboFull));
    other.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::TurboFull);
    REQUIRE(other.action() != Action::TurboFull);
    REQUIRE_FALSE(other.turboBegan());
}

TEST_CASE("the guard comes up while it is asked for, blocks once it is up, and is let down",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        tree.sequences.push_back(sequence);
    };
    add("DEFEND1", 6);
    add("DEFEND2", 20);
    add("DEFENDR", 6);
    add("ATTPWRB", 12);
    add("SHOVE", 8);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Defend);
    REQUIRE(animator.action() == Action::DefendRaise);
    REQUIRE(animator.guarding());
    REQUIRE_FALSE(animator.defending()); // not yet
    REQUIRE(animator.moveScale() == 0.0f);
    s32 steps = 0;
    while (animator.action() != Action::Defend && steps < 60) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Defend);
        ++steps;
    }
    REQUIRE(animator.defending());
    for (s32 i = 0; i < 200; ++i) { // held, it stays up
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Defend);
        REQUIRE(animator.defending());
    }
    // A turbo attack cuts into it; afterwards the guard can come up again.
    REQUIRE(animator.canBegin(PlayerDeed::TurboStrong));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::TurboStrong);
    REQUIRE(animator.action() == Action::TurboStrong);
    REQUIRE_FALSE(animator.guarding());
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Defend);
    REQUIRE(animator.action() == Action::DefendRaise);
    // Let go, it is lowered and the stance comes back.
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(animator.action() == Action::DefendLower);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);
    REQUIRE_FALSE(animator.guarding());
    // A charge rushes on rather than standing.
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Shove);
    REQUIRE(animator.shoving());
    REQUIRE(animator.moveScale() == PlayerAnimator::kChargePace);
    // A class without the sequences does not guard.
    const TreeInfo plain = classTree();
    PlayerAnimator other;
    REQUIRE(other.bind(plain, false));
    other.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Defend);
    REQUIRE_FALSE(other.guarding());
}

TEST_CASE("the strong throw lets the weapon go as its wind-up ends, then recovers",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        tree.sequences.push_back(sequence);
    };
    add("ATTPWRATHROW", 10);
    add("ATTPWRATHROWR", 8);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    REQUIRE(animator.canBegin(PlayerDeed::StrongAttack));
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::StrongAttack);
    REQUIRE(animator.action() == Action::StrongThrow);
    REQUIRE(animator.turboBegan());
    REQUIRE(animator.strongThrowing());
    REQUIRE(animator.moveScale() == PlayerAnimator::kStrongThrowPace);
    s32 releases = 0;
    s32 steps = 0;
    while (animator.strongThrowing() && steps < 200) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::StrongAttack);
        if (animator.strongReleased()) {
            REQUIRE(animator.action() == Action::StrongThrowRecover);
            ++releases;
        }
        ++steps;
    }
    REQUIRE(releases == 1);
    REQUIRE_FALSE(animator.strongThrowing());
    REQUIRE_FALSE(animator.released()); // not an ordinary throw
    // A class without the sequence has no strong throw.
    const TreeInfo plain = classTree();
    PlayerAnimator other;
    REQUIRE(other.bind(plain, false));
    REQUIRE_FALSE(other.canBegin(PlayerDeed::StrongAttack));
}

TEST_CASE("strafing steps in two halves the way it goes, shoots as it goes, and falls can floor it",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        tree.sequences.push_back(sequence);
    };
    for (const char* name : {"STRAFE_WLKF1", "STRAFE_WLKF2", "STRAFE_WLKB1", "STRAFE_WLKB2",
                             "STRAFE_WLKL1", "STRAFE_WLKL2", "STRAFE_WLKR1", "STRAFE_WLKR2",
                             "STRAFE_ATKF1", "STRAFE_ATKF2", "STRAFE_ATKB1", "STRAFE_ATKB2",
                             "STRAFE_ATKL1", "STRAFE_ATKL2", "STRAFE_ATKR1", "STRAFE_ATKR2"}) {
        add(name, 6);
    }
    add("FALLDOWN", 8);
    add("GETUP", 8);
    add("FALLFRNT", 8);
    add("GETUP2", 8);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setStrafe(StrafeWay::Left);
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    REQUIRE(animator.action() == Action::StrafeLeft1);
    REQUIRE(animator.strafing());
    REQUIRE(animator.moveScale() == 1.0f);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::StrafeLeft2, 60) < 60);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::StrafeLeft1, 60) < 60);
    // Another way is taken up at the end of the step; standing still, it stands.
    animator.setStrafe(StrafeWay::Back);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::StrafeBack1, 60) < 60);
    // An attack asked of it is made as it goes, one let fly as each half begins.
    s32 shots = 0;
    for (s32 i = 0; i < 120; ++i) {
        animator.update(PlayerMotion::Walk, kTicks, kStep, true);
        shots += animator.released() ? 1 : 0;
        REQUIRE_FALSE(animator.throwing()); // its feet are never planted for it
    }
    REQUIRE(animator.action() >= Action::StrafeShootBack1);
    REQUIRE(animator.action() <= Action::StrafeShootBack2);
    REQUIRE(shots >= 4);
    animator.setStrafe(StrafeWay::None);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);

    // Floored, it falls, gets up the way it fell, and heeds nothing meanwhile.
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::FallForward);
    REQUIRE(animator.action() == Action::FallForward);
    REQUIRE(animator.floored());
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == 0.0f);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::FallBack); // not felled again
    REQUIRE(animator.action() == Action::FallForward);
    REQUIRE(stepsUntilAttack(animator, Action::GetUpForward, 120) < 120);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 120) < 120);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::FallBack);
    REQUIRE(animator.action() == Action::FallBack);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::GetUpBack, 120) < 120);
}

TEST_CASE("a shield potion is raised with the gesture of a potion used, and told apart",
          "[game][players][animation]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    bool shielded = false;
    bool used = false;
    for (s32 i = 0; i < 200; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ShieldPotion);
        shielded = shielded || animator.potionShielded();
        used = used || animator.potionUsed();
    }
    REQUIRE(shielded);
    REQUIRE_FALSE(used);
    // One a press, as with any potion.
    s32 again = 0;
    for (s32 i = 0; i < 200; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ShieldPotion);
        again += animator.potionShielded() ? 1 : 0;
    }
    REQUIRE(again == 0);
}

TEST_CASE("a legend item is let go of with a potion's, the strong throw's or the special "
          "shot's gesture, and nothing else leaves the hand",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        sequence.repeats = false;
        tree.sequences.push_back(sequence);
    };
    add("ATTPWRATHROW", 10);
    add("ATTPWRATHROWR", 8);
    add("SSHOT1", 11);
    add("SSHOTR", 29);
    struct Gesture {
        PlayerDeed deed;
        Action windUp;
        Action recover;
    };
    const std::array<Gesture, 3> gestures{{
        {PlayerDeed::HurlLegend, Action::UsePotion, Action::UsePotionRelease},
        {PlayerDeed::ThrowLegend, Action::StrongThrow, Action::StrongThrowRecover},
        {PlayerDeed::ShootLegend, Action::SpecialShot, Action::SpecialShotRecover},
    }};
    for (const Gesture& gesture : gestures) {
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        REQUIRE(animator.canBegin(gesture.deed));
        animator.update(PlayerMotion::Run, kTicks, kStep, gesture.deed);
        REQUIRE(animator.action() == gesture.windUp);
        REQUIRE(animator.castingLegend());
        REQUIRE_FALSE(animator.turboBegan()); // the meter pays nothing
        s32 releases = 0;
        s32 steps = 0;
        while (animator.castingLegend() && steps < 200) {
            animator.update(PlayerMotion::Run, kTicks, kStep);
            REQUIRE_FALSE(animator.potionUsed());
            REQUIRE_FALSE(animator.potionShielded());
            REQUIRE_FALSE(animator.strongReleased());
            REQUIRE_FALSE(animator.released());
            if (animator.legendReleased()) {
                REQUIRE(animator.action() == gesture.recover);
                ++releases;
            }
            ++steps;
        }
        REQUIRE(releases == 1);
        REQUIRE(steps < 200);
        REQUIRE_FALSE(animator.conjuring());
        REQUIRE_FALSE(animator.turboing());
    }
    // A class without the special shot has no gesture for it, and a body in the middle of
    // something waits.
    const TreeInfo plain = classTree();
    PlayerAnimator other;
    REQUIRE(other.bind(plain, false));
    REQUIRE_FALSE(other.canBegin(PlayerDeed::ShootLegend));
    REQUIRE(other.canBegin(PlayerDeed::HurlLegend));
    other.update(PlayerMotion::Stand, kTicks, kStep, true);
    REQUIRE(other.throwing());
    REQUIRE_FALSE(other.canBegin(PlayerDeed::HurlLegend));
}

} // namespace
