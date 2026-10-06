#include <algorithm>
#include <array>
#include <format>
#include <vector>

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

TEST_CASE("native quick and strafing shots retain wall-time cadence with speed items",
          "[game][players][animation][alpha-fire-cadence][assets]") {
    for (const auto* code : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        const auto path = test::assetOrSkip(std::format("PLAYERS/{}/ANIM/ANIM.PS2", code));
        AnimationSet actions;
        REQUIRE(actions.load(path.parent_path()));
        const auto found = actions.find(code);
        REQUIRE(found);
        for (s32 power = 0; power < 4; ++power) {
            for (s32 mode = 0; mode < 3; ++mode) {
                std::vector<s32> nativeShots;
                for (const s32 hz : {30, 60, 120, 240}) {
                    CAPTURE(code, power, mode, hz);
                    const f32 seconds = 1.0f / static_cast<f32>(hz);
                    const s32 ticks = std::max(1, 60 / hz);
                    PlayerAnimator animator;
                    REQUIRE(animator.bind(actions.tree(*found), false));
                    animator.setAttackSpeed((power & 1) != 0, (power & 2) != 0);
                    const auto motion = mode == 0 ? PlayerMotion::Stand : PlayerMotion::Run;
                    if (mode == 2) {
                        animator.setStrafe(StrafeWay::Left);
                    }
                    if (mode != 0) {
                        animator.update(motion, ticks, seconds);
                    }
                    animator.update(motion, ticks, seconds, PlayerDeed::Attack);
                    REQUIRE(animator.action() == (mode == 2   ? Action::StrafeShootLeft1
                                                  : mode == 1 ? Action::ThrowMoving
                                                              : Action::Throw));
                    std::vector<s32> shots;
                    for (s32 tick = 1; tick <= hz * 5 && shots.size() < 5; ++tick) {
                        animator.update(motion, ticks, seconds, PlayerDeed::Attack);
                        if (animator.released()) {
                            shots.push_back(tick * (240 / hz));
                        }
                    }
                    REQUIRE(shots.size() == 5);
                    if (hz == 30) {
                        nativeShots = shots;
                    } else {
                        CHECK(shots == nativeShots);
                    }
                }
            }
        }
    }
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
    // it. MBEndFrame caps dispatches below 30 Hz, despite its 60 Hz clock. The
    // 60 Hz poses remain fractional, but THROW1 starts at the next nominal
    // native boundary after frame two, not on the intervening visual sample.
    for (const f32 frame : {0.5f, 1.0f, 1.5f, 2.0f, 2.5f}) {
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

TEST_CASE("a fresh guard cancels ordinary attacks but not turbo item combo or magic actions",
          "[game][players][animation][native-guard][native-guard-cancel]") {
    struct Request {
        PlayerDeed deed;
        Action action;
        bool cancellable;
    };
    // DoPlayerAction 800AC0C4..800AC0F0 dispatches a fresh P_DEFEND1 (119)
    // as Ready when the current PlayerAttackType is 1..10. In particular,
    // ATTPWRATHROW and ATTFIREL/R are ordinary category-10 attacks, unlike
    // ATTPWRB/C, SSHOT, ATTCHOP and ATTBREATHE (11), or combos (12).
    const std::array requests{Request{PlayerDeed::Attack, Action::Throw, true},
                              Request{PlayerDeed::Melee, Action::Quick1, true},
                              Request{PlayerDeed::MeleeLow, Action::LowKick, true},
                              Request{PlayerDeed::MeleeSlow, Action::SlowStart, true},
                              Request{PlayerDeed::MeleeSlowLow, Action::Low1, true},
                              Request{PlayerDeed::StrongAttack, Action::StrongThrow, true},
                              Request{PlayerDeed::FireLeft, Action::FireLeft, true},
                              Request{PlayerDeed::FireRight, Action::FireRight, true},
                              Request{PlayerDeed::Shove, Action::Shove, true},
                              Request{PlayerDeed::TurboStrong, Action::TurboStrong, false},
                              Request{PlayerDeed::TurboFull, Action::TurboFull, false},
                              Request{PlayerDeed::SuperShot, Action::SpecialShot, false},
                              Request{PlayerDeed::Hammer, Action::Hammer, false},
                              Request{PlayerDeed::Breathe, Action::Breathe, false},
                              Request{PlayerDeed::Combo, Action::ComboAct1, false},
                              Request{PlayerDeed::UsePotion, Action::UsePotion, false},
                              Request{PlayerDeed::ThrowPotion, Action::ThrowPotion, false},
                              Request{PlayerDeed::Flinch, Action::HitReact, false},
                              Request{PlayerDeed::FallBack, Action::FallBack, false}};
    const auto tree = actionTree();
    for (const auto& request : requests) {
        CAPTURE(request.deed, request.action, request.cancellable);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, 1, kStep, request.deed);
        REQUIRE(animator.action() == request.action);
        const auto generation = animator.player().generation();
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Defend);
        CHECK(animator.action() == (request.cancellable ? Action::DefendRaise : request.action));
        CHECK((animator.player().generation() != generation) == request.cancellable);
        CHECK_FALSE(animator.released());
        CHECK_FALSE(animator.meleeStruck());
        CHECK_FALSE(animator.strongReleased());
        CHECK_FALSE(animator.superReleased());
        CHECK(animator.itemReleased() == PlayerDeed::None);
    }
}

TEST_CASE("a released guard latches native armor duration only for its blocking phase",
          "[game][players][animation][player-animation-speed][native-guard]") {
    const auto tree = actionTree();
    for (const f32 armor : {0.0f, 1.25f, 2.5f, 5.0f}) {
        for (const bool speed : {false, true}) {
            CAPTURE(armor, speed);
            PlayerAnimator animator;
            REQUIRE(animator.bind(tree, false));
            animator.setGuardArmor(armor);
            animator.setAttackSpeed(false, speed);
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Defend);
            REQUIRE(animator.action() == Action::DefendRaise);
            CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f));
            for (s32 tick = 0; tick < 60 && animator.action() == Action::DefendRaise; ++tick) {
                animator.update(PlayerMotion::Stand, 1, kStep);
            }
            REQUIRE(animator.action() == Action::Defend);
            // Native 800AD318: selected DEFEND2 beats Speed's fallback, but
            // the initial category-1 guard request leaves the raise at rate 1.
            const f32 duration = std::max(0.25f, 0.2f * armor);
            CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f * duration));
            animator.setGuardArmor(9.0f);
            animator.update(PlayerMotion::Stand, 1, kStep);
            REQUIRE(animator.action() == Action::Defend);
            CHECK(animator.player().secondsPerFrame() == Approx(45.0f / 900.0f * duration));
            for (s32 tick = 0; tick < 60 && animator.action() == Action::Defend; ++tick) {
                animator.update(PlayerMotion::Stand, 1, kStep);
            }
            REQUIRE(animator.action() == Action::DefendLower);
            CHECK(animator.player().secondsPerFrame() ==
                  Approx(45.0f / 900.0f * (speed ? 0.75f : 1.0f)));
        }
    }
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

TEST_CASE("stance transitions start with native tick credit without accelerating with Speed",
          "[game][players][animation][player-animation-speed][player-stance-timing]") {
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
        // InitAnim 8000EE80..8000EEAC subtracts one base tick from both start
        // and deadline, independent of animscale. CalcAnimInfo runs immediately.
        constexpr f32 kInitialCredit = 1.0f / 30.0f;
        CHECK(animator.player().transition() == Approx(0.5f));
        CHECK(animator.player().frame() == 0.0f);
        for (s32 tick = 1; tick <= 2; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
            CHECK(animator.player().transition() ==
                  Approx((kInitialCredit + static_cast<f32>(tick) * kStep) /
                         PlayerAnimator::kStanceBlend));
            CHECK(animator.player().frame() == 0.0f);
        }
        CHECK_FALSE(animator.player().transitioning());
    }
}

TEST_CASE("player actions only blend back to stance where the native action table allows it",
          "[game][players][animation][player-stance-timing]") {
    struct Request {
        PlayerDeed deed;
        bool blend;
    };
    // DoPlayerAction 800ACFD8..800AD018 excludes native actions86..147,
    // HIT_REACT27, SPIKE_HIT129 and KNOCKBACK130 from the Ready blend.
    const std::array requests{
        Request{PlayerDeed::Flinch, false},      Request{PlayerDeed::Reel, false},
        Request{PlayerDeed::Spike, false},       Request{PlayerDeed::TurboStrong, false},
        Request{PlayerDeed::TurboFull, false},   Request{PlayerDeed::StrongAttack, false},
        Request{PlayerDeed::Attack, false},      Request{PlayerDeed::UsePotion, false},
        Request{PlayerDeed::ThrowPotion, false}, Request{PlayerDeed::SuperShot, false},
        Request{PlayerDeed::Hammer, false},      Request{PlayerDeed::Breathe, false},
        Request{PlayerDeed::FireLeft, false},    Request{PlayerDeed::FireRight, false},
        Request{PlayerDeed::Defend, false},      Request{PlayerDeed::Gag, false},
        Request{PlayerDeed::Shove, true},        Request{PlayerDeed::Pick, true},
        Request{PlayerDeed::MeleeSlow, true},    Request{PlayerDeed::MeleeLow, true}};
    const auto tree = actionTree();
    for (const auto& request : requests) {
        CAPTURE(request.deed);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, 1, kStep, request.deed);
        REQUIRE(animator.action() != Action::Ready);
        for (s32 tick = 0; tick < 240 && animator.action() != Action::Ready; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == Action::Ready);
        CHECK(animator.player().transitioning() == request.blend);
        CHECK(animator.player().transition() == Approx(request.blend ? 0.5f : 1.0f));
    }
}

TEST_CASE("running changes to walking at a completed native half stride, not immediately",
          "[game][players][animation][player-locomotion-timing]") {
    const auto tree = actionTree();
    for (const bool secondHalf : {false, true}) {
        CAPTURE(secondHalf);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Run, 1, kStep);
        if (secondHalf) {
            for (s32 tick = 0; tick < 60 && animator.action() != Action::Run2; ++tick) {
                animator.update(PlayerMotion::Run, 1, kStep);
            }
        }
        const auto stride = secondHalf ? Action::Run2 : Action::Run1;
        REQUIRE(animator.action() == stride);
        // P_RUN/P_RUN2 keep mode0 for next=P_WALK. CalcAnimInfo's smooth
        // one-shot end is frames-1+.5, here5.5 * (45/900) = .275 seconds.
        for (s32 tick = 1; tick <= 16; ++tick) {
            animator.update(PlayerMotion::Walk, 1, kStep);
            CHECK(animator.action() == stride);
            CHECK(animator.footfall() == PlayerAnimator::Foot::None);
        }
        animator.update(PlayerMotion::Walk, 1, kStep);
        CHECK(animator.action() == Action::Walk1);
        CHECK(animator.footfall() ==
              (secondHalf ? PlayerAnimator::Foot::Second : PlayerAnimator::Foot::First));
        CHECK_FALSE(animator.player().transitioning());
    }
}

TEST_CASE("Archer alone omits the Ready blend after its second quick recovery",
          "[game][players][animation][player-stance-timing]") {
    const auto tree = actionTree();
    for (const s32 character : {0, 3}) {
        CAPTURE(character);
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setCharacter(character);
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Melee);
        for (s32 tick = 0; tick < 60 && animator.action() == Action::Quick1; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Melee);
        }
        REQUIRE(animator.action() == Action::Quick2);
        for (s32 tick = 0; tick < 60 && animator.action() == Action::Quick2; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == Action::Quick2Recover);
        for (s32 tick = 0; tick < 60 && animator.action() != Action::Ready; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == Action::Ready);
        CHECK(animator.player().transitioning() == (character != 3));
        CHECK(animator.player().transition() == Approx(character == 3 ? 1.0f : 0.5f));
    }
}

TEST_CASE("native spike reactions play HITREACT rather than the unused SPIKEHIT sequence",
          "[game][players][animation][alpha-spike-sequence][assets]") {
    // The action enum is not the sequence name: main.dol's names[129] at
    // 80126E6C points to 8011538C (HITREACT), just like names[130].
    for (const auto* code : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        CAPTURE(code);
        const auto path = test::assetOrSkip(std::format("PLAYERS/{}/ANIM/ANIM.PS2", code));
        AnimationSet actions;
        REQUIRE(actions.load(path.parent_path()));
        const auto found = actions.find(code);
        REQUIRE(found);
        const auto& tree = actions.tree(*found);
        const auto hit = tree.findSequence("HITREACT");
        REQUIRE(hit);
        const auto unused = tree.findSequence("SPIKEHIT");
        if (unused) {
            REQUIRE(hit != unused);
        }
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Spike);
        REQUIRE(animator.action() == Action::SpikeHit);
        CHECK(animator.player().sequence() == *hit);
        CHECK_FALSE(animator.player().transitioning());
        for (s32 step = 0; step < 240 && animator.action() == Action::SpikeHit; ++step) {
            CAPTURE(step);
            CHECK_FALSE(animator.released());
            CHECK_FALSE(animator.meleeStruck());
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == Action::Ready);
        CHECK_FALSE(animator.player().transitioning());
    }
}

} // namespace
