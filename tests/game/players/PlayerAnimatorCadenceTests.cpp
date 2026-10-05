#include <array>

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

constexpr f32 kStep = 1.0f / 60.0f;

TreeInfo stanceTree(s32 rate) {
    TreeInfo tree;
    tree.name = "STANCE";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    TreeSequenceInfo sequence;
    sequence.name = "READY";
    sequence.frames = 60;
    sequence.frameRate = rate;
    sequence.repeats = true;
    TrackInfo track;
    track.node = 0;
    track.flags = TrackInfo::channelBit(3);
    track.frames = {0, 59};
    track.values = {0, 59};
    sequence.tracks.push_back(track);
    sequence.trackOfNode = {0};
    tree.sequences.push_back(sequence);
    return tree;
}

TreeInfo actionTree() {
    auto tree = stanceTree(45);
    const auto prototype = tree.sequences.front();
    for (usize i = 1; i < PlayerAnimator::kActionCount; ++i) {
        auto sequence = prototype;
        sequence.name = PlayerAnimator::kSequenceNames[i];
        sequence.frames = 6;
        sequence.repeats = false;
        tree.sequences.push_back(sequence);
    }
    return tree;
}

TEST_CASE("player actions retain native fractional frames without changing fast sequence snapping",
          "[game][players][animation][player-smooth]") {
    struct Sample {
        s32 rate;
        f32 frame;
    };
    // DoPlayerAction 800AC0A4 enables AnimationInfo.flags bit 2 unconditionally.
    // CalcAnimInfo 8000EF18 keeps fractional frames unless within 0.125 of an
    // integer or the scaled frame duration is below the 1/30 base tick.
    const std::array samples{Sample{30, 0.5f}, Sample{45, 1.0f / 3.0f}, Sample{24, 1.0f}};
    for (const auto& sample : samples) {
        CAPTURE(sample.rate);
        const auto tree = stanceTree(sample.rate);
        PlayerAnimator animator;
        for (s32 binding = 0; binding < 2; ++binding) {
            CAPTURE(binding);
            REQUIRE(animator.bind(tree, false));
            animator.update(PlayerMotion::Stand, 1, kStep);
            CHECK(animator.player().frame() == Approx(sample.frame));
            CHECK(animator.pose().matrices()[0][3].x == Approx(sample.frame));
            CHECK(animator.action() == Action::Ready);
            CHECK_FALSE(animator.released());
            animator.unbind();
        }
    }
}

TEST_CASE(
    "Sorceress native quick throws wait for windup frame two rather than rounded frame one half",
    "[game][players][animation][player-smooth][assets]") {
    const auto path = test::assetOrSkip("PLAYERS/SOR/ANIM/ANIM.PS2");
    AnimationSet actions;
    REQUIRE(actions.load(path.parent_path()));
    const auto found = actions.find("SOR");
    REQUIRE(found);
    const auto& tree = actions.tree(*found);
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
    REQUIRE(animator.action() == Action::Throw);
    const auto& windup = tree.sequences[animator.player().sequence()];
    REQUIRE(windup.name == "THROW1S");
    REQUIRE(windup.frames == 7);
    REQUIRE(windup.frameRate == 30);
    CHECK(animator.player().frame() == 0.0f);

    // Native action dispatch examines the old frame before AnimateTree updates
    // it. At one hardware tick per update this reaches 1.5, then 2, and only
    // the following dispatch may cut into THROW1. At 30 Hz there is no half
    // frame, so that authored two-tick cadence is unchanged.
    for (const f32 frame : {0.5f, 1.0f, 1.5f, 2.0f}) {
        CAPTURE(frame);
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
        CHECK(animator.action() == Action::Throw);
        CHECK(animator.player().frame() == Approx(frame));
        CHECK_FALSE(animator.released());
    }
    animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
    CHECK(animator.action() == Action::ThrowRelease);
    CHECK(animator.player().frame() == 0.0f);
    CHECK_FALSE(animator.released());
}

TEST_CASE("powered player animation snapping uses its effective frame duration",
          "[game][players][animation][player-smooth]") {
    struct Sample {
        s32 rate;
        f32 frame;
    };
    // The native 0.75 animation scale makes rate 30's frames 1/40 second,
    // below CalcAnimInfo's snap threshold. Rate 45 remains slower than 1/30,
    // so one hardware tick instead retains the fractional 4/9 frame.
    const std::array samples{Sample{30, 1.0f}, Sample{45, 4.0f / 9.0f}};
    for (const auto& sample : samples) {
        auto tree = stanceTree(sample.rate);
        auto windup = tree.sequences.front();
        windup.name = "THROW1S";
        windup.repeats = false;
        tree.sequences.push_back(windup);
        for (const bool rapid : {false, true}) {
            CAPTURE(sample.rate, rapid);
            PlayerAnimator animator;
            REQUIRE(animator.bind(tree, false));
            animator.setAttackSpeed(rapid, !rapid);
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            REQUIRE(animator.action() == Action::Throw);
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            CHECK(animator.player().frame() == Approx(sample.frame));
            CHECK(animator.pose().matrices()[0][3].x == Approx(sample.frame));
            CHECK_FALSE(animator.released());

            // A new class/body must not inherit a faster sequence's snap mode.
            REQUIRE(animator.bind(tree, false));
            animator.update(PlayerMotion::Stand, 1, kStep);
            const f32 normal = sample.rate == 30 ? 0.5f : 1.0f / 3.0f;
            CHECK(animator.player().frame() == Approx(normal));
        }
    }
}

TEST_CASE("player animation item changes latch at the next sequence start",
          "[game][players][animation][player-animation-speed]") {
    const auto tree = actionTree();
    // InitAnim 8000EE14..8000EE2C copies animscale into seqscale. CalcAnimInfo
    // reads the cached value, so an item toggle cannot retime a running sequence.
    constexpr f32 kNativePeriod = 45.0f / 900.0f;
    for (const bool initiallyPowered : {false, true}) {
        for (const bool rapid : {false, true}) {
            CAPTURE(initiallyPowered, rapid);
            PlayerAnimator changed;
            PlayerAnimator unchanged;
            REQUIRE(changed.bind(tree, false));
            REQUIRE(unchanged.bind(tree, false));
            changed.setAttackSpeed(rapid && initiallyPowered, !rapid && initiallyPowered);
            unchanged.setAttackSpeed(rapid && initiallyPowered, !rapid && initiallyPowered);
            changed.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            unchanged.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            REQUIRE(changed.action() == Action::Throw);
            const auto generation = changed.player().generation();
            const f32 currentPeriod = kNativePeriod * (initiallyPowered ? 0.75f : 1.0f);
            CHECK(changed.player().secondsPerFrame() == Approx(currentPeriod));
            changed.setAttackSpeed(rapid && !initiallyPowered, !rapid && !initiallyPowered);
            for (s32 tick = 0; tick < 2; ++tick) {
                changed.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
                unchanged.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
                CHECK(changed.player().generation() == generation);
                CHECK(changed.player().frame() == Approx(unchanged.player().frame()));
                CHECK(changed.player().secondsPerFrame() == Approx(currentPeriod));
            }
            for (s32 tick = 0; tick < 30 && changed.action() == Action::Throw; ++tick) {
                changed.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            }
            REQUIRE(changed.action() == Action::ThrowRelease);
            CHECK(changed.player().secondsPerFrame() ==
                  Approx(kNativePeriod * (initiallyPowered ? 1.0f : 0.75f)));
        }
    }
}

TEST_CASE("rapid fire uses the requested attack when a throw proceeds after button release",
          "[game][players][animation][player-animation-speed]") {
    const auto tree = actionTree();
    PlayerAnimator held;
    PlayerAnimator released;
    REQUIRE(held.bind(tree, false));
    REQUIRE(released.bind(tree, false));
    held.setAttackSpeed(true, false);
    released.setAttackSpeed(true, false);
    held.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
    released.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
    REQUIRE(held.action() == Action::Throw);
    REQUIRE(released.action() == Action::Throw);
    for (s32 tick = 0; tick < 30 && held.action() == Action::Throw; ++tick) {
        held.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
        released.update(PlayerMotion::Stand, 1, kStep);
        CHECK(held.action() == released.action());
        CHECK(held.player().frame() == Approx(released.player().frame()));
        CHECK_FALSE(released.released());
    }
    REQUIRE(held.action() == Action::ThrowRelease);
    REQUIRE(released.action() == Action::ThrowRelease);
    // DoPlayerAction uses PlayerAttackType(next), not the selected continuation
    // act: READY's type 0 no longer qualifies for Rapid Fire's types 9 or 10.
    CHECK(held.player().secondsPerFrame() == Approx(45.0f / 900.0f * 0.75f));
    CHECK(released.player().secondsPerFrame() == Approx(45.0f / 900.0f));
    s32 shots = 0;
    for (s32 tick = 0; tick < 120 && released.action() != Action::Ready; ++tick) {
        released.update(PlayerMotion::Stand, 1, kStep);
        shots += released.released() ? 1 : 0;
    }
    CHECK(released.action() == Action::Ready);
    CHECK(shots == 1);
}

TEST_CASE("player item animation scale follows native requested action categories without stacking",
          "[game][players][animation][player-animation-speed]") {
    struct Request {
        PlayerDeed deed = PlayerDeed::None;
        Action action = Action::Ready;
        bool rapid = false;
        bool speed = false;
        StrafeWay strafe = StrafeWay::None;
        bool enter = false;
    };
    // PlayerAttackType 800ADBFC: normal throws are 9, strong throws/gauntlets
    // 10, shooting strafes 7. Defense/shove are 1; turbo/item/combo are >=11.
    const std::array requests{
        Request{PlayerDeed::Attack, Action::Throw, true, true},
        Request{PlayerDeed::StrongAttack, Action::StrongThrow, true, true},
        Request{PlayerDeed::FireLeft, Action::FireLeft, true, true},
        Request{PlayerDeed::FireRight, Action::FireRight, true, true},
        Request{PlayerDeed::Attack, Action::StrafeShootLeft1, false, true, StrafeWay::Left},
        Request{PlayerDeed::Defend, Action::DefendRaise, false, false},
        Request{PlayerDeed::Shove, Action::Shove, false, false},
        Request{PlayerDeed::TurboFull, Action::TurboFull, false, false},
        Request{PlayerDeed::SuperShot, Action::SpecialShot, false, false},
        Request{PlayerDeed::Hammer, Action::Hammer, false, false},
        Request{PlayerDeed::Breathe, Action::Breathe, false, false},
        Request{PlayerDeed::Combo, Action::ComboAct1, false, false},
        Request{PlayerDeed::UsePotion, Action::UsePotion, false, true},
        Request{PlayerDeed::Flinch, Action::HitReact, false, true},
        Request{PlayerDeed::None, Action::Start, false, true, StrafeWay::None, true}};
    const auto tree = actionTree();
    for (const auto& request : requests) {
        for (const bool rapid : {false, true}) {
            for (const bool speed : {false, true}) {
                CAPTURE(request.action, rapid, speed);
                PlayerAnimator animator;
                REQUIRE(animator.bind(tree, request.enter));
                animator.setAttackSpeed(rapid, speed);
                animator.setStrafe(request.strafe);
                const auto motion =
                    request.strafe == StrafeWay::None ? PlayerMotion::Stand : PlayerMotion::Walk;
                animator.update(motion, 1, kStep, request.deed);
                REQUIRE(animator.action() == request.action);
                const f32 scale =
                    (rapid && request.rapid) || (speed && request.speed) ? 0.75f : 1.0f;
                CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f * scale));
            }
        }
    }
}

TEST_CASE("selected combo continuations remain unscaled after their button is released",
          "[game][players][animation][player-animation-speed]") {
    const auto tree = actionTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setAttackSpeed(true, true);
    animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Combo);
    REQUIRE(animator.action() == Action::ComboAct1);
    for (s32 tick = 0; tick < 60 && animator.action() == Action::ComboAct1; ++tick) {
        animator.update(PlayerMotion::Stand, 1, kStep);
    }
    REQUIRE(animator.action() == Action::ComboAct2);
    CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f));
    animator.update(PlayerMotion::Stand, 1, kStep);
    REQUIRE(animator.action() == Action::ComboAct3);
    CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f));
}

TEST_CASE("locked strong and item attacks preserve held requests for continuation speed",
          "[game][players][animation][player-animation-speed][player-locked-speed]") {
    struct Attack {
        PlayerDeed deed = PlayerDeed::None;
        Action first = Action::Ready;
        Action next = Action::Ready;
        bool rapid = false;
    };
    const std::array attacks{
        Attack{PlayerDeed::StrongAttack, Action::StrongThrow, Action::StrongThrowRecover, true},
        Attack{PlayerDeed::FireLeft, Action::FireLeft, Action::FireLeftRecover, true},
        Attack{PlayerDeed::FireRight, Action::FireRight, Action::FireRightRecover, true},
        Attack{PlayerDeed::SuperShot, Action::SpecialShot, Action::SpecialShotRepeat, false},
        Attack{PlayerDeed::Hammer, Action::Hammer, Action::HammerRecover, false},
        Attack{PlayerDeed::Breathe, Action::Breathe, Action::BreatheRecover, false}};
    const auto tree = actionTree();
    for (const auto& attack : attacks) {
        for (const bool held : {false, true}) {
            CAPTURE(attack.first, held);
            PlayerAnimator animator;
            REQUIRE(animator.bind(tree, false));
            // Native input-to-action selection retains the held request even
            // when the current animation owns the body. Speed is deliberately
            // present only for the type-11 requests, which must exclude it.
            animator.setAttackSpeed(attack.rapid, !attack.rapid);
            animator.update(PlayerMotion::Stand, 1, kStep, attack.deed);
            REQUIRE(animator.action() == attack.first);
            for (s32 tick = 0; tick < 60 && animator.action() == attack.first; ++tick) {
                animator.update(PlayerMotion::Stand, 1, kStep,
                                held ? attack.deed : PlayerDeed::None);
            }
            const auto next = !held && attack.deed == PlayerDeed::SuperShot
                                  ? Action::SpecialShotRecover
                                  : attack.next;
            REQUIRE(animator.action() == next);
            const f32 scale = held == attack.rapid ? 0.75f : 1.0f;
            CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f * scale));
        }
    }
}

TEST_CASE("Speed changes sequence duration without accelerating the stance transition clock",
          "[game][players][animation][player-animation-speed]") {
    const auto tree = actionTree();
    for (const bool speed : {false, true}) {
        CAPTURE(speed);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setAttackSpeed(false, speed);
        animator.update(PlayerMotion::Walk, 1, kStep);
        REQUIRE(animator.action() == Action::Walk1);
        for (s32 tick = 0; tick < 60 && animator.action() != Action::Ready; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == Action::Ready);
        REQUIRE(animator.player().transitioning());
        // InitAnim's transition deadline is wall-clock time, independent of
        // animscale. Test the supplied blend interval, not its native initial
        // one-base-tick credit (a separate transition-start convention).
        for (s32 tick = 1; tick <= 4; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
            CHECK(animator.player().transition() ==
                  Approx(static_cast<f32>(tick) * kStep / PlayerAnimator::kStanceBlend));
        }
        CHECK_FALSE(animator.player().transitioning());
    }
}

} // namespace
