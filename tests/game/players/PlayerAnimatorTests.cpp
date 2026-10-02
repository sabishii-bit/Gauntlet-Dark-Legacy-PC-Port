#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
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
    const std::array<Entry, 24> entries{
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
         {"SPIKEHIT", 15, 30, false},    {"WEBREACT", 15, 30, false}}};
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

TEST_CASE("attack invulnerability follows turbo and combo animations through recovery",
          "[player-animation][attack-invulnerability]") {
    TreeInfo tree;
    tree.name = "WAR";
    tree.nodes.emplace_back();
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 6;
        sequence.frameRate = 30;
        tree.sequences.push_back(sequence);
    }
    struct Case {
        PlayerDeed deed;
        Action action;
        bool protectedAttack;
    };
    const std::array cases{Case{PlayerDeed::TurboFull, Action::TurboFull, true},
                           Case{PlayerDeed::TurboStrong, Action::TurboStrong, true},
                           Case{PlayerDeed::Combo, Action::ComboAct1, true},
                           Case{PlayerDeed::ComboHeld, Action::ComboWar1, true},
                           Case{PlayerDeed::SuperShot, Action::SpecialShot, true},
                           Case{PlayerDeed::Hammer, Action::Hammer, true},
                           Case{PlayerDeed::Breathe, Action::Breathe, true},
                           Case{PlayerDeed::StrongAttack, Action::StrongThrow, false},
                           Case{PlayerDeed::Shove, Action::Shove, false},
                           Case{PlayerDeed::FireLeft, Action::FireLeft, false},
                           Case{PlayerDeed::FireRight, Action::FireRight, false},
                           Case{PlayerDeed::UsePotion, Action::UsePotion, false}};
    // player_can_be_damaged + PlayerAttackType: the >=11 attack groups, not every
    // action which locks input. In particular ordinary strong throws remain vulnerable.
    for (const auto& entry : cases) {
        CAPTURE(entry.action);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setCombo(0, false);
        CHECK_FALSE(animator.damageProtected());
        animator.update(PlayerMotion::Stand, kTicks, kStep, entry.deed);
        REQUIRE(animator.action() == entry.action);
        s32 frames = 0;
        while (animator.action() != Action::Ready && frames < 100) {
            CHECK(animator.damageProtected() == entry.protectedAttack);
            animator.update(PlayerMotion::Stand, kTicks, kStep);
            ++frames;
        }
        REQUIRE(frames > 0);
        REQUIRE(animator.action() == Action::Ready);
        CHECK_FALSE(animator.damageProtected());
    }
}

TEST_CASE("super shots repeat their firing cycle and interruptions never release a shot",
          "[game][items][player-animation]") {
    auto tree = classTree();
    for (const auto* name : {"SSHOT1", "SSHOT2", "SSHOTR"}) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 6;
        sequence.frameRate = 30;
        tree.sequences.push_back(sequence);
    }
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, 2, kStep, PlayerDeed::SuperShot);
    CHECK(animator.action() == Action::SpecialShot);
    CHECK_FALSE(animator.turboBegan());
    CHECK_FALSE(animator.superReleased());
    SECTION("holding fires every completed cycle; release recovers") {
        s32 shots = 0;
        for (s32 i = 0; i < 30; ++i) {
            animator.update(PlayerMotion::Stand, 2, kStep, PlayerDeed::SuperShot);
            shots += animator.superReleased() ? 1 : 0;
            CHECK_FALSE(animator.released());
            CHECK_FALSE(animator.legendReleased());
        }
        CHECK(shots >= 3);
        for (s32 i = 0; i < 30; ++i) {
            animator.update(PlayerMotion::Stand, 2, kStep);
        }
        CHECK(animator.action() == Action::Ready);
    }
    SECTION("damage cancels windup") {
        animator.update(PlayerMotion::Stand, 2, kStep, PlayerDeed::Flinch);
        CHECK(animator.action() == Action::HitReact);
        CHECK_FALSE(animator.superReleased());
    }
    SECTION("death cancels even on a finishing frame") {
        animator.update(PlayerMotion::Stand, 60, 1, PlayerDeed::Die);
        CHECK_FALSE(animator.superReleased());
    }
}

TEST_CASE("rapid fire speeds throwing but not arrival or idle", "[game][items][player-animation]") {
    const auto tree = classTree();
    PlayerAnimator normal;
    PlayerAnimator rapid;
    REQUIRE(normal.bind(tree, false));
    REQUIRE(rapid.bind(tree, false));
    rapid.setAttackSpeed(true, false);
    s32 normalShots = 0;
    s32 rapidShots = 0;
    for (s32 i = 0; i < 180; ++i) {
        normal.update(PlayerMotion::Stand, 2, kStep, PlayerDeed::Attack);
        rapid.update(PlayerMotion::Stand, 2, kStep, PlayerDeed::Attack);
        normalShots += normal.released() ? 1 : 0;
        rapidShots += rapid.released() ? 1 : 0;
    }
    CHECK(rapidShots > normalShots);
    REQUIRE(normal.bind(tree));
    REQUIRE(rapid.bind(tree));
    rapid.setAttackSpeed(true, true);
    for (s32 i = 0; i < 10; ++i) {
        normal.update(PlayerMotion::Stand, 2, kStep);
        rapid.update(PlayerMotion::Stand, 2, kStep);
        CHECK(normal.player().frame() == rapid.player().frame());
    }
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

TEST_CASE("sticky contacts use WEBREACT without restarting the reaction every frame",
          "[game][players][animation][player-impact][wraith]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Attack);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Webbed);
    REQUIRE(animator.action() == Action::WebReact);
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == Approx(0.4f));
    REQUIRE_FALSE(animator.released());
    for (s32 i = 0; i < 120; ++i) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Webbed);
        REQUIRE(animator.action() == Action::WebReact);
        REQUIRE(animator.moveScale() == Approx(0.4f));
    }
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

TEST_CASE("potion throw strength counts held wind-up ticks and stops at release",
          "[game][players][animation][potion-throw]") {
    const TreeInfo tree = classTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
    REQUIRE(animator.potionThrowTicks() == 0);
    for (s32 frame = 0; frame < 4; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
    }
    REQUIRE(animator.potionThrowTicks() == 8);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
    for (s32 frame = 0; frame < 60 && !animator.potionThrown(); ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
    }
    REQUIRE(animator.potionThrown());
    REQUIRE(animator.potionThrowTicks() == 8); // a second press cannot resume this throw's charge
    for (s32 frame = 0; frame < 60; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
    }
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
    REQUIRE(animator.potionThrowTicks() == 0);
    s32 held = 0;
    for (s32 frame = 0; frame < 60 && !animator.potionThrown(); ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ThrowPotion);
        held += kTicks;
    }
    REQUIRE(animator.potionThrown());
    REQUIRE(animator.potionThrowTicks() == held);
    REQUIRE(held > 8);
    animator.unbind();
    REQUIRE(animator.potionThrowTicks() == 0);
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
    REQUIRE(animator.entering());
    REQUIRE(animator.moveScale() == 0);

    // The entrance cuts in at once and, asked to walk, plays out first.
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    REQUIRE(animator.action() == Action::Start);
    REQUIRE(playingIndex(animator) == 4.0f);
    REQUIRE(animator.entering());
    REQUIRE(animator.moveScale() == 0);
    const s32 untilWalk = stepsUntil(animator, PlayerMotion::Walk, Action::Walk1, 200);
    REQUIRE(untilWalk == 60);
    REQUIRE_FALSE(animator.entering());
    REQUIRE(animator.moveScale() == 1);
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
    // A run covers 1.3 of the character's pace (AnimAction's per-action move scale).
    REQUIRE(animator.moveScale() == PlayerAnimator::kRunPace);
    REQUIRE(PlayerAnimator::kRunPace == Approx(1.3f));
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
    REQUIRE(animator.moveScale() == 0);
    s32 releases = 0;
    s32 steps = 0;
    while (animator.strongThrowing() && steps < 200) {
        CHECK(animator.moveScale() == 0); // wind-up and recovery, including held input
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
    // Strafing steps cover two thirds of the pace (AnimAction's per-action move scale).
    REQUIRE(animator.moveScale() == PlayerAnimator::kStrafePace);
    REQUIRE(PlayerAnimator::kStrafePace == Approx(0.667f));
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
    REQUIRE(animator.moveScale() == PlayerAnimator::kStrafePace); // shooting keeps the pace
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

TEST_CASE("a single attack press interrupts either half of every walking strafe",
          "[game][players][animation][strafe-press]") {
    TreeInfo tree = classTree();
    for (auto i = static_cast<usize>(Action::StrafeForward1);
         i <= static_cast<usize>(Action::StrafeShootRight2); ++i) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = PlayerAnimator::kSequenceNames[i];
        sequence.frames = 12;
        tree.sequences.push_back(sequence);
    }
    for (const StrafeWay way :
         {StrafeWay::Forward, StrafeWay::Back, StrafeWay::Left, StrafeWay::Right}) {
        for (s32 phase = 0; phase < 30; ++phase) {
            PlayerAnimator animator;
            REQUIRE(animator.bind(tree, false));
            animator.setStrafe(way);
            for (s32 tick = 0; tick <= phase; ++tick) {
                animator.update(PlayerMotion::Walk, kTicks, kStep);
            }
            INFO("direction " << static_cast<s32>(way) << " phase " << phase);
            animator.update(PlayerMotion::Walk, kTicks, kStep, PlayerDeed::Attack);
            REQUIRE(animator.released());
            REQUIRE(animator.action() == PlayerAnimator::strafeStep(way, true));
            animator.update(PlayerMotion::Walk, kTicks, kStep);
            REQUIRE_FALSE(animator.released());
        }
    }
}

TEST_CASE("retail class animations accept lateral strafe attack taps throughout both steps",
          "[game][players][animation][strafe-press][unpacked]") {
    for (const char* code : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        const auto path =
            test::unpackedOrSkip(std::string("PLAYERS/") + code + "/ANIM/animations.json");
        AnimationSet actions;
        REQUIRE(actions.load(path.parent_path()));
        const auto found = actions.find(code);
        REQUIRE(found);
        for (const StrafeWay way : {StrafeWay::Left, StrafeWay::Right}) {
            for (s32 phase = 0; phase < 60; ++phase) {
                PlayerAnimator animator;
                REQUIRE(animator.bind(actions.tree(*found), false));
                animator.setStrafe(way);
                for (s32 tick = 0; tick <= phase; ++tick) {
                    animator.update(PlayerMotion::Run, kTicks, kStep);
                }
                INFO(code << " direction " << static_cast<s32>(way) << " phase " << phase);
                animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
                REQUIRE(animator.released());
                REQUIRE(animator.action() == PlayerAnimator::strafeStep(way, true));
            }
        }
    }
}

TEST_CASE("a whirlwind flings the body up once and it gets up after (action.c 1270)",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    for (const char* name : {"FLYUP", "GETUP"}) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = 8;
        tree.sequences.push_back(sequence);
    }
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Whirled);
    REQUIRE(animator.action() == Action::Whirled);
    REQUIRE(animator.floored());
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == 0.0f);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Whirled); // not restarted
    REQUIRE(animator.action() == Action::Whirled);
    REQUIRE(stepsUntilAttack(animator, Action::GetUpBack, 120) < 120);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 120) < 120);
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

TEST_CASE("captured players loop GRABBED and hold their fall until released by physics",
          "[game][players][animation][yeti]") {
    TreeInfo tree = classTree();
    for (const auto& name : {"GRABBED", "FALLDOWN", "GETUP"}) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = 4;
        tree.sequences.push_back(sequence);
    }
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Attack);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Grabbed);
    REQUIRE(animator.action() == Action::Grabbed);
    REQUIRE(animator.reacting());
    REQUIRE(animator.moveScale() == 0);
    for (s32 frame = 0; frame < 20; ++frame) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Grabbed);
        REQUIRE(animator.action() == Action::Grabbed);
        REQUIRE_FALSE(animator.released());
    }
    for (s32 frame = 0; frame < 20; ++frame) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Thrown);
        REQUIRE(animator.action() == Action::FallBack);
        REQUIRE_FALSE(animator.released());
    }
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(animator.action() == Action::GetUpBack);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 60) < 60);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Grabbed);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Die);
    REQUIRE(animator.dying());
}

TreeInfo meleeTree() {
    TreeInfo tree = classTree();
    for (auto action = static_cast<usize>(Action::Quick1); action < PlayerAnimator::kActionCount;
         ++action) {
        TreeSequenceInfo sequence;
        sequence.name = PlayerAnimator::kSequenceNames[action];
        sequence.frames = 6;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

TEST_CASE("item animation events separate breath entry from hammer and gauntlet impact",
          "[game][items][animation]") {
    const TreeInfo tree = meleeTree();
    for (const auto deed :
         {PlayerDeed::Hammer, PlayerDeed::Breathe, PlayerDeed::FireLeft, PlayerDeed::FireRight}) {
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, kTicks, kStep, deed);
        REQUIRE(animator.itemAttacking());
        CHECK_FALSE(animator.turboBegan());
        CHECK(animator.moveScale() == 0);
        s32 emitted = animator.itemReleased() == deed ? 1 : 0;
        CHECK(emitted == (deed == PlayerDeed::Breathe ? 1 : 0));
        for (s32 frame = 0; frame < 30; ++frame) {
            animator.update(PlayerMotion::Stand, kTicks, kStep);
            emitted += animator.itemReleased() == deed ? 1 : 0;
            CHECK_FALSE(animator.released());
            CHECK_FALSE(animator.strongReleased());
            CHECK_FALSE(animator.superReleased());
        }
        CHECK(emitted == 1);
        CHECK(animator.action() == Action::Ready);
        animator.update(PlayerMotion::Stand, kTicks, kStep, deed);
        animator.update(PlayerMotion::Stand, 30, 0.5f, PlayerDeed::Die);
        CHECK(animator.itemReleased() == PlayerDeed::None);
    }
}

TEST_CASE("held close attacks chain quick swings without ever throwing a weapon",
          "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Melee);
    REQUIRE(animator.action() == Action::Quick1);
    REQUIRE(animator.moveScale() == PlayerAnimator::kQuickMeleePace);
    s32 hits = 0;
    for (s32 frame = 0; frame < 30; ++frame) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Melee);
        CHECK_FALSE(animator.released());
        CHECK_FALSE(animator.strongReleased());
        if (animator.meleeStruck()) {
            ++hits;
            CHECK(animator.action() == (hits % 2 != 0 ? Action::Quick2 : Action::Quick3));
            CHECK(animator.meleeBlow() == MeleeBlow::Plain);
        }
    }
    REQUIRE(hits >= 3);
    for (s32 frame = 0; frame < 25; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
        CHECK_FALSE(animator.released());
    }
    CHECK(animator.action() == Action::Ready);
    CHECK_FALSE(animator.meleeStruck());
}

TEST_CASE("slow and low melee contacts occur once per completed swing and cancel on damage",
          "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    for (const PlayerDeed deed :
         {PlayerDeed::MeleeSlow, PlayerDeed::MeleeLow, PlayerDeed::MeleeSlowLow}) {
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, kTicks, kStep, deed);
        REQUIRE(animator.meleeing());
        s32 contacts = 0;
        for (s32 frame = 0; frame < 30; ++frame) {
            animator.update(PlayerMotion::Stand, kTicks, kStep);
            if (animator.meleeStruck()) {
                ++contacts;
                MeleeBlow expected = MeleeBlow::Plain;
                if (deed == PlayerDeed::MeleeSlow) {
                    expected = MeleeBlow::Heavy;
                } else if (deed == PlayerDeed::MeleeLow) {
                    expected = MeleeBlow::Kick;
                }
                CHECK(animator.meleeBlow() == expected);
            }
            CHECK_FALSE(animator.released());
            CHECK_FALSE(animator.strongReleased());
        }
        CHECK(contacts == 1);
        CHECK(animator.action() == Action::Ready);
    }
    for (const PlayerDeed interrupt : {PlayerDeed::Flinch, PlayerDeed::Die, PlayerDeed::Reel}) {
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        animator.update(PlayerMotion::Stand, 30, 0.5f, interrupt);
        CHECK_FALSE(animator.meleeStruck());
        CHECK_FALSE(animator.meleeing());
    }
}

/** Steps with `deed` until the body leaves `action`, at most `limit` updates. */
Action playOut(PlayerAnimator& animator, Action action, PlayerDeed deed, s32 limit = 30) {
    for (s32 frame = 0; frame < limit && animator.action() == action; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, deed);
    }
    return animator.action();
}

TEST_CASE("a strong press within a chain of swings makes the chain's power swing",
          "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    // First swing, then a strong press: the close power swing, three times the harm.
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
    REQUIRE(animator.action() == Action::Quick1);
    CHECK(animator.meleeChain() == 1);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
    REQUIRE(playOut(animator, Action::Quick1, PlayerDeed::None) == Action::PowerClose);
    CHECK(animator.turboBegan());
    CHECK(animator.meleeBlow() == MeleeBlow::Plain); // the swing that gave way to it
    CHECK(animator.moveScale() == Approx(0.5f));
    s32 power = 0;
    for (s32 frame = 0; frame < 30; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
        power += animator.meleeBlow() == MeleeBlow::Power ? 1 : 0;
    }
    CHECK(power == 1);
    CHECK(animator.action() == Action::Ready);
    CHECK(animator.meleeChain() == 0);

    // Tapped: each fresh press counts, so the second strong press makes the medium one and
    // the third the spin.
    for (const auto& [taps, expected] :
         {std::pair{2, Action::PowerMed}, std::pair{3, Action::Spin}}) {
        PlayerAnimator tapped;
        REQUIRE(tapped.bind(tree, false));
        tapped.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        for (s32 tap = 1; tap < taps; ++tap) {
            const Action swing = tapped.action();
            tapped.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
            tapped.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
            REQUIRE(playOut(tapped, swing, PlayerDeed::Melee) != swing);
        }
        CHECK(tapped.meleeChain() == taps);
        tapped.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::None);
        tapped.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
        CHECK(playOut(tapped, tapped.action(), PlayerDeed::None) == expected);
    }

    // Held without pressing again, the chain starts over and a strong press is not buffered:
    // it only carries the swings on.
    PlayerAnimator held;
    REQUIRE(held.bind(tree, false));
    held.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
    REQUIRE(playOut(held, Action::Quick1, PlayerDeed::Melee) == Action::Quick2);
    CHECK(held.meleeChain() == 0);
    held.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
    CHECK(playOut(held, Action::Quick2, PlayerDeed::None) == Action::Quick3);
    CHECK(held.meleeChain() == 1);

    // A buffered strong press after a kick makes the low power swing.
    PlayerAnimator kick;
    REQUIRE(kick.bind(tree, false));
    kick.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeLow);
    REQUIRE(kick.action() == Action::LowKick);
    kick.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
    CHECK(playOut(kick, Action::LowKick, PlayerDeed::None) == Action::PowerLow);
}

TEST_CASE("swings turn to where their target lies", "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    struct Case {
        f32 yaw;
        Action first;  ///< from the first swing
        Action second; ///< in place of the second
    };
    for (const Case& c :
         {Case{2.5f, Action::Turn, Action::Turn2}, Case{-2.5f, Action::TurnLeft, Action::TurnLeft2},
          Case{1.2f, Action::Right, Action::Right2}, Case{-1.2f, Action::Left, Action::Left2},
          Case{0.5f, Action::Quick1, Action::Quick2}}) {
        CAPTURE(c.yaw);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setMelee(MeleeSense{MeleeRange::Swing, false, c.yaw});
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        CHECK(animator.action() == c.first);
        CHECK(animator.turnScale() == Approx(c.first == Action::Quick1 ? 0.0f : 1.0f));
        s32 plain = 0;
        for (s32 frame = 0; frame < 30; ++frame) {
            animator.update(PlayerMotion::Stand, kTicks, kStep);
            plain += animator.meleeBlow() == MeleeBlow::Plain ? 1 : 0;
        }
        CHECK(plain == 1);
        CHECK(animator.action() == Action::Ready);
        // The second swing of a chain turns with its own sequences.
        animator.setMelee(MeleeSense{});
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        animator.setMelee(MeleeSense{MeleeRange::Swing, false, c.yaw});
        const Action first = animator.action();
        CHECK(playOut(animator, first, PlayerDeed::Melee) == c.second);
    }
}

TEST_CASE("a step carries a swing to what is a pace away", "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setMelee(MeleeSense{MeleeRange::Step, false, 0.0f});
    // Standing still, a thing a pace away is not stepped to.
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
    CHECK(animator.action() == Action::Quick1);
    PlayerAnimator stepping;
    REQUIRE(stepping.bind(tree, false));
    stepping.setMelee(MeleeSense{MeleeRange::Step, false, 0.0f});
    stepping.update(PlayerMotion::Walk, kTicks, kStep, PlayerDeed::Melee);
    REQUIRE(stepping.action() == Action::Step1);
    CHECK(stepping.lunging());
    CHECK(stepping.moveScale() == Approx(1.0f));
    CHECK(stepping.turnScale() == Approx(0.25f));
    stepping.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
    CHECK(stepping.moveScale() == Approx(0.5f)); // on at half pace with the stick let go
    // Still a pace away, the chain steps again; within a swing it swings.
    CHECK(playOut(stepping, Action::Step1, PlayerDeed::Melee) == Action::Step2);
    CHECK(stepping.meleeBlow() == MeleeBlow::Heavy);
    stepping.setMelee(MeleeSense{});
    CHECK(playOut(stepping, Action::Step2, PlayerDeed::Melee) == Action::Quick3);

    // Cut in from the second half of a walk, the step is the walking strike.
    PlayerAnimator walker;
    REQUIRE(walker.bind(tree, false));
    walker.update(PlayerMotion::Walk, kTicks, kStep);
    for (s32 frame = 0; frame < 30 && walker.action() != Action::Walk2; ++frame) {
        walker.update(PlayerMotion::Walk, kTicks, kStep);
    }
    REQUIRE(walker.action() == Action::Walk2);
    walker.setMelee(MeleeSense{MeleeRange::Step, false, 0.0f});
    walker.update(PlayerMotion::Walk, kTicks, kStep, PlayerDeed::Melee);
    CHECK(walker.action() == Action::WalkStrike);
}

TEST_CASE("power swings pace the classes as they did, and the low one borrows the close one's",
          "[game][players][animation][melee]") {
    TreeInfo tree = meleeTree();
    const auto lowFirst = std::ranges::find(tree.sequences, "ATTPWRALOW", &TreeSequenceInfo::name);
    REQUIRE(lowFirst != tree.sequences.end());
    tree.sequences.erase(lowFirst, lowFirst + 2); // ATTPWRALOW and ATTPWRALOWR, as the knight
    struct Case {
        s32 character;
        f32 pace;
        f32 turn;
    };
    // Warrior, wizard, knight, sorceress.
    for (const Case& c :
         {Case{0, 0.5f, 1.0f}, Case{2, 0.25f, 1.0f}, Case{5, 0.0f, 0.0f}, Case{6, 0.0f, 0.0f}}) {
        CAPTURE(c.character);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setCharacter(c.character);
        animator.setMelee(MeleeSense{MeleeRange::Swing, true, 0.0f});
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
        REQUIRE(playOut(animator, Action::Quick1, PlayerDeed::None) == Action::PowerLow);
        CHECK(animator.sequenceOf(Action::PowerLow) == animator.sequenceOf(Action::PowerClose));
        CHECK(animator.moveScale() == Approx(0.25f));
        CHECK(animator.turnScale() == Approx(1.0f));
        PlayerAnimator close;
        REQUIRE(close.bind(tree, false));
        close.setCharacter(c.character);
        close.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
        close.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::MeleeSlow);
        REQUIRE(playOut(close, Action::Quick1, PlayerDeed::None) == Action::PowerClose);
        CHECK(close.moveScale() == Approx(c.pace));
        CHECK(close.turnScale() == Approx(c.turn));
    }
}

TEST_CASE("a turbo move cuts a close attack off", "[game][players][animation][melee]") {
    const TreeInfo tree = meleeTree();
    TreeInfo withTurbo = tree;
    TreeSequenceInfo turbo;
    turbo.name = "ATTPWRB";
    turbo.frames = 20;
    turbo.frameRate = 30;
    turbo.trackOfNode = {-1};
    withTurbo.sequences.push_back(turbo);
    turbo.name = "ATTPWRATHROW";
    withTurbo.sequences.push_back(turbo);
    PlayerAnimator animator;
    REQUIRE(animator.bind(withTurbo, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Melee);
    REQUIRE(animator.meleeing());
    CHECK(animator.canBegin(PlayerDeed::TurboStrong));
    CHECK_FALSE(animator.canBegin(PlayerDeed::StrongAttack));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::TurboStrong);
    CHECK(animator.action() == Action::TurboStrong);
    CHECK_FALSE(animator.meleeStruck());
}

TEST_CASE("a pickup's gesture plays through at full pace; a gag loops while it lasts",
          "[game][players][animation][pickup]") {
    const TreeInfo tree = meleeTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Pick);
    REQUIRE(animator.action() == Action::Pick);
    CHECK(animator.moveScale() == Approx(1.0f));
    CHECK_FALSE(animator.reacting());
    // An attack waits for the gesture's end (P_PICKUP).
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Attack);
    CHECK(animator.action() == Action::Pick);
    CHECK(playOut(animator, Action::Pick, PlayerDeed::Attack) == Action::Throw);

    // Retching loops while asked for and, let go of, ends its cycle.
    PlayerAnimator gag;
    REQUIRE(gag.bind(tree, false));
    gag.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Gag);
    REQUIRE(gag.action() == Action::Gag);
    for (s32 frame = 0; frame < 20; ++frame) {
        gag.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Gag);
        CHECK(gag.action() == Action::Gag);
    }
    CHECK(playOut(gag, Action::Gag, PlayerDeed::None) == Action::Ready);
    // A walk cuts in only past its first frames.
    PlayerAnimator walker;
    REQUIRE(walker.bind(tree, false));
    walker.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Gag);
    walker.update(PlayerMotion::Walk, kTicks, kStep);
    CHECK(walker.action() == Action::Gag);
}

TEST_CASE("a shield on the arm has its own stance and a gait at a run's pace",
          "[game][players][animation][shield]") {
    const TreeInfo tree = meleeTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setShielded(true);
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    CHECK(animator.action() == Action::ShieldReady);
    for (s32 frame = 0; frame < 20; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep);
        CHECK(animator.action() == Action::ShieldReady); // looping, never fidgeting
    }
    animator.update(PlayerMotion::Walk, kTicks, kStep);
    CHECK(animator.action() == Action::ShieldRun);
    CHECK(animator.moveScale() == Approx(PlayerAnimator::kRunPace));
    for (s32 frame = 0; frame < 20; ++frame) {
        animator.update(PlayerMotion::Walk, kTicks, kStep);
        CHECK(animator.action() == Action::ShieldRun);
    }
    animator.setShielded(false);
    CHECK(playOut(animator, Action::ShieldRun, PlayerDeed::None) == Action::Ready);
}

TEST_CASE("a halo reaches out to Death, holds him in a loop and lets go",
          "[game][players][animation][death]") {
    const TreeInfo tree = meleeTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Walk, kTicks, kStep, PlayerDeed::DeathGrab);
    REQUIRE(animator.action() == Action::DeathGrabStart); // at once, from anything
    CHECK(animator.grabbingDeath());
    CHECK(animator.moveScale() == 0.0f);
    CHECK(playOut(animator, Action::DeathGrabStart, PlayerDeed::DeathGrab) == Action::DeathGrab);
    for (s32 frame = 0; frame < 20; ++frame) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::DeathGrab);
        CHECK(animator.action() == Action::DeathGrab);
    }
    animator.update(PlayerMotion::Stand, kTicks, kStep);
    CHECK(animator.action() == Action::DeathGrabRelease);
    CHECK(playOut(animator, Action::DeathGrabRelease, PlayerDeed::None) == Action::Ready);
}

TEST_CASE("a body pushed along shows it instead of standing or walking",
          "[game][players][animation]") {
    TreeInfo tree = classTree();
    TreeSequenceInfo sequence = tree.sequences.front();
    sequence.name = "PUSHED";
    sequence.frames = 8;
    tree.sequences.push_back(sequence);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setPushed(true);
    REQUIRE(stepsUntil(animator, PlayerMotion::Walk, Action::Pushed, 60) < 60);
    for (s32 i = 0; i < 30; ++i) { // looped while it lasts
        animator.update(PlayerMotion::Stand, kTicks, kStep);
        REQUIRE(animator.action() == Action::Pushed);
    }
    REQUIRE(animator.moveScale() == 1.0f);
    // A throw is not pushed aside.
    animator.update(PlayerMotion::Stand, kTicks, kStep, true);
    REQUIRE(animator.throwing());
    animator.setPushed(false);
    REQUIRE(stepsUntil(animator, PlayerMotion::Stand, Action::Ready, 120) < 120);
}

/** The class tree with the combo sequences of the given lengths. */
TreeInfo comboTree(bool secondAct) {
    TreeInfo tree = classTree();
    const auto add = [&tree](const char* name, s32 frames, bool repeats = false) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = frames;
        sequence.repeats = repeats;
        tree.sequences.push_back(sequence);
    };
    add("COMBOACT1", 12);
    if (secondAct) {
        add("COMBOACT2", 6, true);
        add("COMBOACT3", 8);
    }
    add("COMBOWAR1", 12);
    add("COMBOWAR2", 4, true);
    add("COMBOWAR3", 6);
    add("COMBOVAL", 12);
    add("COMBODWF1", 12);
    add("COMBODWF2", 4, true);
    add("COMBODWF3", 6);
    return tree;
}

TEST_CASE("the combo move cuts in, plays its acts through unheeding, and the dwarf's second act "
          "loops while the ride lasts",
          "[game][players][animation][combo]") {
    const TreeInfo warrior = comboTree(false);
    PlayerAnimator animator;
    REQUIRE(animator.bind(warrior, false));
    REQUIRE(animator.canBegin(PlayerDeed::Combo));
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 10) < 10);
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Combo);
    REQUIRE(animator.action() == Action::ComboAct1);
    REQUIRE(animator.turboBegan());
    REQUIRE(animator.comboing());
    REQUIRE(animator.turboing());
    REQUIRE(animator.moveScale() == 0.0f);
    REQUIRE(animator.turnScale() == 0.0f);
    REQUIRE_FALSE(animator.canBegin(PlayerDeed::TurboFull));
    REQUIRE_FALSE(animator.comboTakeable());
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::ComboAct1);
    // With no second act it ends into whatever is asked.
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 30) < 30);
    REQUIRE(animator.comboTakeable());
    // The dwarf's: the first act gives way to the second, which loops while the ride is
    // asked for and then cuts to the third at once.
    const TreeInfo dwarf = comboTree(true);
    PlayerAnimator rider;
    REQUIRE(rider.bind(dwarf, false));
    rider.setCombo(-1, true);
    rider.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::Combo);
    REQUIRE(rider.action() == Action::ComboAct1);
    REQUIRE(stepsUntil(rider, PlayerMotion::Stand, Action::ComboAct2, 30) < 30);
    for (s32 i = 0; i < 20; ++i) {
        rider.update(PlayerMotion::Stand, kTicks, kStep);
        REQUIRE(rider.action() == Action::ComboAct2);
    }
    rider.setCombo(-1, false);
    rider.update(PlayerMotion::Stand, kTicks, kStep);
    REQUIRE(rider.action() == Action::ComboAct3);
    REQUIRE(rider.comboing());
    REQUIRE(stepsUntil(rider, PlayerMotion::Stand, Action::Ready, 30) < 30);
    // A class without COMBOACT1 has no combo.
    const TreeInfo plain = classTree();
    PlayerAnimator none;
    REQUIRE(none.bind(plain, false));
    REQUIRE_FALSE(none.canBegin(PlayerDeed::Combo));
}

TEST_CASE("a partner held plays the grabber's class sequence once, thrown loops its flight and "
          "lands when let go",
          "[game][players][animation][combo]") {
    const TreeInfo tree = comboTree(false);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 10) < 10);
    animator.setCombo(0, false); // in a warrior's hands
    animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::ComboHeld);
    REQUIRE(animator.action() == Action::ComboWar1);
    REQUIRE(animator.comboBound());
    REQUIRE(animator.moveScale() == 0.0f);
    REQUIRE_FALSE(animator.comboTakeable());
    REQUIRE_FALSE(animator.canBegin(PlayerDeed::Combo));
    // Held on to its end, the sequence holds its last frame.
    for (s32 i = 0; i < 30; ++i) {
        animator.update(PlayerMotion::Run, kTicks, kStep, PlayerDeed::ComboHeld);
        REQUIRE(animator.action() == Action::ComboWar1);
    }
    // Let fly, it loops the pinball; let go of, it lands at once and then stands.
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
    REQUIRE(animator.action() == Action::ComboWar2);
    REQUIRE(animator.comboThrown());
    for (s32 i = 0; i < 20; ++i) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
        REQUIRE(animator.action() == Action::ComboWar2);
    }
    animator.update(PlayerMotion::Run, kTicks, kStep);
    REQUIRE(animator.action() == Action::ComboWar3);
    REQUIRE(stepsUntil(animator, PlayerMotion::Run, Action::Run1, 30) < 30);
    // Which sequence is by the grabber's class; a class with no thrown sequence is only held.
    animator.setCombo(1, false);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboHeld);
    REQUIRE(animator.action() == Action::ComboVal);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
    REQUIRE(animator.action() == Action::ComboVal);
    REQUIRE(PlayerAnimator::comboHeldActionOf(4) == Action::ComboDwf1);
    REQUIRE(PlayerAnimator::comboThrownActionOf(4) == Action::ComboDwf2);
    REQUIRE(PlayerAnimator::comboThrownActionOf(7) == Action::Ready);
    REQUIRE(PlayerAnimator::comboHeldActionOf(9) == Action::Ready);
    REQUIRE(animator.turnScale() == 0.0f);
    // The held sequence plays out before the flight takes over (action.c 1008's mode 0).
    animator.setCombo(4, false);
    s32 frames = 0;
    while (animator.action() == Action::ComboVal && frames < 30) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
        ++frames;
    }
    REQUIRE(animator.action() == Action::ComboDwf2);
    REQUIRE(frames > 0);
    REQUIRE(frames <= 12);
    REQUIRE(animator.turnScale() == Approx(0.5f));
}

TEST_CASE("the real class trees carry the combo sequences the original's table names",
          "[game][players][animation][combo][unpacked]") {
    struct Expected {
        const char* code;
        Action held;
        bool thrown;
    };
    const std::array<Expected, 8> classes{{{"WAR", Action::ComboWar1, true},
                                           {"VAL", Action::ComboVal, false},
                                           {"WIZ", Action::ComboWiz, false},
                                           {"ARC", Action::ComboArc, false},
                                           {"DWF", Action::ComboDwf1, true},
                                           {"KNI", Action::ComboKni, false},
                                           {"SOR", Action::ComboSor, false},
                                           {"JES", Action::ComboJes, false}}};
    for (usize c = 0; c < classes.size(); ++c) {
        const auto path = test::unpackedOrSkip(std::string("PLAYERS/") + classes[c].code +
                                               "/ANIM/animations.json");
        AnimationSet actions;
        REQUIRE(actions.load(path.parent_path()));
        const auto found = actions.find(classes[c].code);
        REQUIRE(found.has_value());
        const TreeInfo& tree = actions.tree(*found);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        REQUIRE(animator.canBegin(PlayerDeed::Combo));
        REQUIRE(animator.sequenceOf(Action::ComboAct1) != animator.sequenceOf(Action::Ready));
        // Only the dwarf has the second and third acts.
        const bool dwarf = std::string(classes[c].code) == "DWF";
        REQUIRE((animator.sequenceOf(Action::ComboAct2) != animator.sequenceOf(Action::Ready)) ==
                dwarf);
        animator.setCombo(static_cast<s32>(c), false);
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboHeld);
        REQUIRE(animator.action() == classes[c].held);
        // Let fly, the held sequence plays out and then the flight loops, for the warrior's
        // and the dwarf's partners alone.
        for (s32 frame = 0; frame < 100 && !animator.comboThrown(); ++frame) {
            animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
        }
        REQUIRE(animator.comboThrown() == classes[c].thrown);
    }
    // The warrior's partner is let fly once COMBOWAR1 has played (its thirty frames).
    const auto path = test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
    AnimationSet actions;
    REQUIRE(actions.load(path.parent_path()));
    const auto warrior = actions.find("WAR");
    REQUIRE(warrior.has_value());
    PlayerAnimator animator;
    REQUIRE(animator.bind(actions.tree(*warrior), false));
    animator.setCombo(0, false);
    animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboHeld);
    REQUIRE(animator.action() == Action::ComboWar1);
    s32 frames = 0;
    while (animator.action() == Action::ComboWar1 && frames < 60) {
        animator.update(PlayerMotion::Stand, kTicks, kStep, PlayerDeed::ComboThrown);
        ++frames;
    }
    REQUIRE(animator.action() == Action::ComboWar2);
    REQUIRE(frames == 30);
}

} // namespace
