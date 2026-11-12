#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "TestSupport.h"
#include "game/characters/ClassData.h"
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
                    REQUIRE(animator.bind(actions.tree(*found), Entrance::Skip));
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
            REQUIRE(animator.bind(tree, Entrance::Skip));
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
    REQUIRE(animator.bind(tree, Entrance::Skip));
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
            REQUIRE(animator.bind(tree, Entrance::Skip));
            animator.setAttackSpeed(rapid, !rapid);
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            REQUIRE(animator.action() == Action::Throw);
            animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
            CHECK(animator.player().frame() == Approx(sample.frame));
            CHECK(animator.pose().matrices()[0][3].x == Approx(sample.frame));
            CHECK_FALSE(animator.released());

            // A new class/body must not inherit a faster sequence's snap mode.
            REQUIRE(animator.bind(tree, Entrance::Skip));
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
            REQUIRE(changed.bind(tree, Entrance::Skip));
            REQUIRE(unchanged.bind(tree, Entrance::Skip));
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
    REQUIRE(held.bind(tree, Entrance::Skip));
    REQUIRE(released.bind(tree, Entrance::Skip));
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
                REQUIRE(animator.bind(tree, request.enter ? Entrance::Play : Entrance::Skip));
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
    REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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
            REQUIRE(animator.bind(tree, Entrance::Skip));
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
            REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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
        REQUIRE(animator.bind(tree, Entrance::Skip));
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

constexpr s32 kLongFrames = 16;         ///< frames of every action in longActionTree
constexpr f32 kSpellStormFrame = 11.0f; ///< the full turbo's last frame that still turns it

/** actionTree with every action's sequence long enough to stand in past kSpellStormFrame. */
TreeInfo longActionTree() {
    auto tree = actionTree();
    for (usize i = 1; i < tree.sequences.size(); ++i) {
        tree.sequences[i].frames = kLongFrames;
    }
    return tree;
}

PlayerAnimator boundTo(const TreeInfo& tree) {
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, Entrance::Skip));
    return animator;
}

/** A copy of `bound` restored into `action` through a copy of its own checkpoint: the body of
 * `character`, the stick moved or not, `frame` frames in, a quick press pending or not. */
PlayerAnimator restoredInto(const PlayerAnimator& bound, Action action, s32 character = -1,
                            bool moved = false, f32 frame = 0.0f, bool pressed = false) {
    auto animator = bound;
    auto state = animator.checkpoint();
    state.action = action;
    state.animation.sequence = animator.sequenceOf(action);
    state.animation.frame = frame;
    state.animation.time = static_cast<f64>(frame) * state.animation.secondsPerFrame;
    state.character = character;
    state.moved = moved;
    state.quickPress = pressed;
    REQUIRE(animator.restore(state));
    return animator;
}

/** One bit for each thing the animator answers of the action it is in, in kPredicates' order. */
namespace answer {
constexpr u32 kComboing = 1U << 0U;
constexpr u32 kComboBound = 1U << 1U;
constexpr u32 kComboThrown = 1U << 2U;
constexpr u32 kComboTakeable = 1U << 3U;
constexpr u32 kGrabbingDeath = 1U << 4U;
constexpr u32 kStrafing = 1U << 5U;
constexpr u32 kFloored = 1U << 6U;
constexpr u32 kThrowing = 1U << 7U;
constexpr u32 kConjuring = 1U << 8U;
constexpr u32 kDying = 1U << 9U;
constexpr u32 kRecovering = 1U << 10U;
constexpr u32 kMeleeing = 1U << 11U;
constexpr u32 kEntering = 1U << 12U;
constexpr u32 kRunning = 1U << 13U;
constexpr u32 kLunging = 1U << 14U;
constexpr u32 kWebbed = 1U << 15U;
constexpr u32 kGuarding = 1U << 16U;
constexpr u32 kDefending = 1U << 17U;
constexpr u32 kShoving = 1U << 18U;
constexpr u32 kTurboing = 1U << 19U;
constexpr u32 kSpecialShooting = 1U << 20U;
constexpr u32 kDamageProtected = 1U << 21U;
constexpr u32 kItemAttacking = 1U << 22U;
constexpr u32 kStrongThrowing = 1U << 23U;
constexpr u32 kReacting = 1U << 24U;
constexpr u32 kCanAutoMelee = 1U << 25U;
constexpr u32 kIsBlockableAttack = 1U << 26U;
constexpr u32 kIsMelee = 1U << 27U;
constexpr u32 kLocomotion = 1U << 28U;
constexpr u32 kRollbackTurboAction = 1U << 29U;
constexpr u32 kRollbackShoveAction = 1U << 30U;
} // namespace answer

/** A predicate the animator answers of its current action, by its bit and name. */
struct Predicate {
    u32 bit;
    std::string_view name;
    bool (*holds)(const PlayerAnimator& animator);
};

constexpr std::array kPredicates{
    Predicate{answer::kComboing, "comboing", [](const PlayerAnimator& a) { return a.comboing(); }},
    Predicate{answer::kComboBound, "comboBound",
              [](const PlayerAnimator& a) { return a.comboBound(); }},
    Predicate{answer::kComboThrown, "comboThrown",
              [](const PlayerAnimator& a) { return a.comboThrown(); }},
    Predicate{answer::kComboTakeable, "comboTakeable",
              [](const PlayerAnimator& a) { return a.comboTakeable(); }},
    Predicate{answer::kGrabbingDeath, "grabbingDeath",
              [](const PlayerAnimator& a) { return a.grabbingDeath(); }},
    Predicate{answer::kStrafing, "strafing", [](const PlayerAnimator& a) { return a.strafing(); }},
    Predicate{answer::kFloored, "floored", [](const PlayerAnimator& a) { return a.floored(); }},
    Predicate{answer::kThrowing, "throwing", [](const PlayerAnimator& a) { return a.throwing(); }},
    Predicate{answer::kConjuring, "conjuring",
              [](const PlayerAnimator& a) { return a.conjuring(); }},
    Predicate{answer::kDying, "dying", [](const PlayerAnimator& a) { return a.dying(); }},
    Predicate{answer::kRecovering, "recovering",
              [](const PlayerAnimator& a) { return a.recovering(); }},
    Predicate{answer::kMeleeing, "meleeing", [](const PlayerAnimator& a) { return a.meleeing(); }},
    Predicate{answer::kEntering, "entering", [](const PlayerAnimator& a) { return a.entering(); }},
    Predicate{answer::kRunning, "running", [](const PlayerAnimator& a) { return a.running(); }},
    Predicate{answer::kLunging, "lunging", [](const PlayerAnimator& a) { return a.lunging(); }},
    Predicate{answer::kWebbed, "webbed", [](const PlayerAnimator& a) { return a.webbed(); }},
    Predicate{answer::kGuarding, "guarding", [](const PlayerAnimator& a) { return a.guarding(); }},
    Predicate{answer::kDefending, "defending",
              [](const PlayerAnimator& a) { return a.defending(); }},
    Predicate{answer::kShoving, "shoving", [](const PlayerAnimator& a) { return a.shoving(); }},
    Predicate{answer::kTurboing, "turboing", [](const PlayerAnimator& a) { return a.turboing(); }},
    Predicate{answer::kSpecialShooting, "specialShooting",
              [](const PlayerAnimator& a) { return a.specialShooting(); }},
    Predicate{answer::kDamageProtected, "damageProtected",
              [](const PlayerAnimator& a) { return a.damageProtected(); }},
    Predicate{answer::kItemAttacking, "itemAttacking",
              [](const PlayerAnimator& a) { return a.itemAttacking(); }},
    Predicate{answer::kStrongThrowing, "strongThrowing",
              [](const PlayerAnimator& a) { return a.strongThrowing(); }},
    Predicate{answer::kReacting, "reacting", [](const PlayerAnimator& a) { return a.reacting(); }},
    Predicate{answer::kCanAutoMelee, "canAutoMelee",
              [](const PlayerAnimator& a) { return a.canAutoMelee(); }},
    Predicate{
        answer::kIsBlockableAttack, "isBlockableAttack",
        [](const PlayerAnimator& a) { return PlayerAnimator::isBlockableAttack(a.action()); }},
    Predicate{answer::kIsMelee, "isMelee",
              [](const PlayerAnimator& a) { return PlayerAnimator::isMelee(a.action()); }},
    Predicate{answer::kLocomotion, "locomotion",
              [](const PlayerAnimator& a) { return PlayerAnimator::locomotion(a.action()); }},
    Predicate{
        answer::kRollbackTurboAction, "rollbackTurboAction",
        [](const PlayerAnimator& a) { return PlayerAnimator::rollbackTurboAction(a.action()); }},
    Predicate{answer::kRollbackShoveAction, "rollbackShoveAction", [](const PlayerAnimator& a) {
                  return PlayerAnimator::rollbackShoveAction(a.action());
              }}};

u32 answersOf(const PlayerAnimator& animator) {
    u32 answers = 0;
    for (const auto& predicate : kPredicates) {
        if (predicate.holds(animator)) {
            answers |= predicate.bit;
        }
    }
    return answers;
}

/** Bit d: whether the deed numbered d may begin now. */
u64 beginsOf(const PlayerAnimator& animator) {
    u64 begins = 0;
    for (usize deed = 0; deed < kPlayerDeedCount; ++deed) {
        if (animator.canBegin(static_cast<PlayerDeed>(deed))) {
            begins |= u64{1} << deed;
        }
    }
    return begins;
}

/** What one tick of a trace asks of the body, and what the body is told before it is asked. */
struct TraceInput {
    PlayerMotion motion = PlayerMotion::Stand;
    PlayerDeed deed = PlayerDeed::None;
    MeleeSense melee;
    StrafeWay strafe = StrafeWay::None;
    s32 comboClass = -1;
    bool ride = false;
    bool shielded = false;
    bool pushed = false;
    bool rapid = false;
    bool speed = false;
    bool chainHeld = false;
};

constexpr s32 kTraceTicks = 90;
constexpr std::array kTraceClasses{0, 2, 3, 5, 6, 7};
constexpr std::array kMotions{PlayerMotion::Stand, PlayerMotion::Walk, PlayerMotion::Run};

/** Every deed but dying, in their order. */
constexpr auto kLivingDeeds = [] {
    std::array<PlayerDeed, kPlayerDeedCount - 1> deeds{};
    usize next = 0;
    for (usize deed = 0; deed < kPlayerDeedCount; ++deed) {
        if (static_cast<PlayerDeed>(deed) != PlayerDeed::Die) {
            deeds[next++] = static_cast<PlayerDeed>(deed);
        }
    }
    return deeds;
}();

/** Nothing asked: what each action gives way to by itself. */
TraceInput standing(s32 /*tick*/) {
    return {};
}

/** Walking, running, then standing, with a shield borne for a while and another member
 * pushing twice. */
TraceInput afoot(s32 tick) {
    TraceInput input;
    input.motion = tick < 30   ? PlayerMotion::Walk
                   : tick < 60 ? PlayerMotion::Run
                               : PlayerMotion::Stand;
    input.shielded = tick >= 20 && tick < 70;
    input.pushed = (tick >= 40 && tick < 50) || (tick >= 75 && tick < 85);
    return input;
}

/** The attack held standing and then walking, with rapid fire and then speed, then let go. */
TraceInput throwing(s32 tick) {
    TraceInput input;
    input.deed = tick < 60 ? PlayerDeed::Attack : PlayerDeed::None;
    input.motion = tick < 30 ? PlayerMotion::Stand : PlayerMotion::Walk;
    input.rapid = tick < 45;
    input.speed = tick >= 45;
    return input;
}

/** A quick close attack pressed every sixth tick at something within a swing. */
TraceInput quickPresses(s32 tick) {
    TraceInput input;
    input.deed = tick % 6 < 3 ? PlayerDeed::Melee : PlayerDeed::None;
    return input;
}

/** Quick and strong presses in turn while walking, at something a step away, then within a
 * swing, then beyond, lying all round. */
TraceInput steppingSwings(s32 tick) {
    constexpr std::array kDeeds{PlayerDeed::Melee, PlayerDeed::Melee,     PlayerDeed::None,
                                PlayerDeed::None,  PlayerDeed::MeleeSlow, PlayerDeed::MeleeSlow,
                                PlayerDeed::None,  PlayerDeed::None};
    constexpr f32 kYawStep = 0.9f;
    TraceInput input;
    input.deed = kDeeds[static_cast<usize>(tick) % kDeeds.size()];
    input.motion = PlayerMotion::Walk;
    input.melee.range = tick < 30   ? MeleeRange::Step
                        : tick < 60 ? MeleeRange::Swing
                                    : MeleeRange::Beyond;
    input.melee.yaw = static_cast<f32>(tick % 7 - 3) * kYawStep;
    return input;
}

/** Low strikes, quick and strong in turn, at something short within a swing to one side. */
TraceInput lowStrikes(s32 tick) {
    TraceInput input;
    const s32 phase = tick % 6;
    input.deed = phase < 2                  ? PlayerDeed::MeleeLow
                 : phase == 3 || phase == 4 ? PlayerDeed::MeleeSlowLow
                                            : PlayerDeed::None;
    input.melee = {MeleeRange::Swing, true, 0.5f};
    return input;
}

/** The quick button held through a chain, as the automatic melee mode carries it, then held
 * low, at something within a swing and then a step away by turns. */
TraceInput heldChains(s32 tick) {
    TraceInput input;
    input.deed = tick < 45 ? PlayerDeed::Melee : PlayerDeed::MeleeLow;
    input.chainHeld = true;
    input.melee.range = tick % 30 < 20 ? MeleeRange::Swing : MeleeRange::Step;
    return input;
}

/** Close contact walking and then running, at something a step away and within a swing by
 * turns. */
TraceInput automaticMelee(s32 tick) {
    TraceInput input;
    input.motion = tick < 45 ? PlayerMotion::Walk : PlayerMotion::Run;
    input.deed = tick % 4 < 2 ? PlayerDeed::AutoMelee : PlayerDeed::AutoMeleeLow;
    input.melee.range = tick % 30 < 15 ? MeleeRange::Step : MeleeRange::Swing;
    return input;
}

/** Every deed but dying for a tick each, standing, walking and running by turns, at something
 * within a swing aside, in a warrior's combo and then a dwarf's, the items on and off. */
TraceInput everyDeed(s32 tick) {
    TraceInput input;
    input.deed = kLivingDeeds[static_cast<usize>(tick) % kLivingDeeds.size()];
    input.motion = kMotions[static_cast<usize>(tick / 5) % kMotions.size()];
    input.melee = {MeleeRange::Swing, tick % 2 == 0, 1.2f};
    input.comboClass = tick < 45 ? 0 : 4;
    input.ride = tick < 70;
    input.rapid = tick % 30 < 15;
    input.speed = tick % 20 < 10;
    return input;
}

/** Every deed but dying held two ticks each while walking, at something a step away, ridden in
 * a dwarf's combo. */
TraceInput heldDeeds(s32 tick) {
    TraceInput input;
    input.deed = kLivingDeeds[static_cast<usize>(tick / 2) % kLivingDeeds.size()];
    input.motion = PlayerMotion::Walk;
    input.melee.range = MeleeRange::Step;
    input.comboClass = 4;
    input.ride = true;
    return input;
}

/** Strafing each way in turn and then not, shooting through the second half of each. */
TraceInput strafing(s32 tick) {
    constexpr std::array kWays{StrafeWay::Forward, StrafeWay::Back, StrafeWay::Left,
                               StrafeWay::Right, StrafeWay::None};
    TraceInput input;
    input.strafe = kWays[static_cast<usize>(tick / 18) % kWays.size()];
    input.motion = PlayerMotion::Walk;
    input.deed = tick % 18 >= 9 ? PlayerDeed::Attack : PlayerDeed::None;
    return input;
}

/** Held in a partner's combo, flung or ridden, then making one's own: a warrior's and then a
 * dwarf's. */
TraceInput combos(s32 tick) {
    TraceInput input;
    const s32 phase = tick % 45;
    input.deed = phase < 15   ? PlayerDeed::ComboHeld
                 : phase < 35 ? PlayerDeed::ComboThrown
                              : PlayerDeed::Combo;
    input.comboClass = tick < 45 ? 0 : 4;
    input.ride = phase < 40;
    return input;
}

/** Walking, then struck dead. */
TraceInput falling(s32 tick) {
    TraceInput input;
    input.motion = PlayerMotion::Walk;
    input.deed = tick < 20 ? PlayerDeed::None : PlayerDeed::Die;
    return input;
}

/** SHA-256, in hex, of what each action does over kTraceTicks of `script` from its first frame,
 * for each of kTraceClasses: the action and sequence, the footfall, every release, the blow,
 * the chain, the presses, the pace and turn, and whether a blend is under way. */
std::string traceDigest(const PlayerAnimator& bound, TraceInput (*script)(s32 tick)) {
    NativeStateDigest digest;
    for (const s32 character : kTraceClasses) {
        for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
            auto animator = restoredInto(bound, static_cast<Action>(i), character);
            for (s32 tick = 0; tick < kTraceTicks; ++tick) {
                const TraceInput input = script(tick);
                animator.setMelee(input.melee);
                animator.setStrafe(input.strafe);
                animator.setCombo(input.comboClass, input.ride);
                animator.setShielded(input.shielded);
                animator.setPushed(input.pushed);
                animator.setAttackSpeed(input.rapid, input.speed);
                animator.update(input.motion, 1, kStep, input.deed, input.chainHeld);
                const auto state = animator.checkpoint();
                digest.values(
                    animator.action(), animator.player().sequence(), animator.footfall(),
                    animator.released(), animator.strongReleased(), animator.superReleased(),
                    animator.legendReleased(), animator.potionUsed(), animator.potionThrown(),
                    animator.potionShielded(), animator.itemReleased(), animator.turboBegan(),
                    animator.dead(), animator.castingLegend(), animator.meleeBlow(),
                    animator.meleeChain(), state.quickPress, state.strongPress,
                    animator.moveScale(), animator.turnScale(), animator.player().transitioning());
            }
        }
    }
    return test::hexBytes(digest.finish());
}

/** A trace's script, and the digest traceDigest gives of it. */
struct TraceScript {
    std::string_view name;
    TraceInput (*input)(s32 tick);
    std::string_view digest;
};

TEST_CASE("each action keeps its answer to every animator predicate and each deed it lets begin",
          "[game][players][player-animation][action-traits]") {
    // Keyed by action, since sequence names repeat (SpikeHit plays HITREACT). The last column is
    // canBegin's answer for each deed, bit d for the deed numbered d.
    struct Row {
        Action action;
        u32 answers;
        u64 begins;
    };
    constexpr std::array<Row, PlayerAnimator::kActionCount> kRows{{
        {Action::Ready, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Idle1, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Idle2, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Idle2Loop, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Walk1, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Walk2, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Run1,
         answer::kComboTakeable | answer::kRunning | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Run2,
         answer::kComboTakeable | answer::kRunning | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::Start, answer::kEntering, 0x0000000000},
        {Action::Throw, answer::kComboTakeable | answer::kThrowing, 0x2070000300},
        {Action::ThrowMoving, answer::kComboTakeable | answer::kThrowing, 0x2070000300},
        {Action::ThrowRelease, answer::kComboTakeable | answer::kThrowing, 0x2070000300},
        {Action::ThrowMovingRelease, answer::kComboTakeable | answer::kThrowing, 0x2070000300},
        {Action::ThrowRecover, answer::kComboTakeable | answer::kThrowing | answer::kRecovering,
         0x2070000300},
        {Action::ThrowMovingRecover,
         answer::kComboTakeable | answer::kThrowing | answer::kRecovering, 0x2070000300},
        {Action::UsePotion, answer::kConjuring | answer::kCanAutoMelee, 0x0000000000},
        {Action::UsePotionRelease, answer::kConjuring | answer::kCanAutoMelee, 0x0000000000},
        {Action::ThrowPotion, answer::kConjuring | answer::kCanAutoMelee, 0x0000000000},
        {Action::ThrowPotionRelease, answer::kConjuring | answer::kCanAutoMelee, 0x0000000000},
        {Action::Death, answer::kDying, 0x0000000000},
        {Action::HitReact, answer::kReacting, 0x0000000000},
        {Action::Stun, answer::kReacting, 0x0000000000},
        {Action::TurboStrong,
         answer::kTurboing | answer::kDamageProtected | answer::kIsBlockableAttack |
             answer::kRollbackTurboAction,
         0x0000000000},
        {Action::TurboFull,
         answer::kTurboing | answer::kDamageProtected | answer::kIsBlockableAttack |
             answer::kRollbackTurboAction,
         0x0000000000},
        {Action::Shove,
         answer::kComboTakeable | answer::kShoving | answer::kTurboing |
             answer::kRollbackShoveAction,
         0x2070000300},
        {Action::DefendRaise, answer::kGuarding, 0x2070050700},
        {Action::Defend, answer::kGuarding | answer::kDefending, 0x2070050700},
        {Action::DefendLower, answer::kGuarding, 0x2070050700},
        {Action::StrongThrow,
         answer::kComboTakeable | answer::kTurboing | answer::kStrongThrowing |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::StrongThrowRecover,
         answer::kComboTakeable | answer::kTurboing | answer::kStrongThrowing |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::StrafeForward1,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeForward2,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeBack1,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeBack2,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeLeft1,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeLeft2,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeRight1,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeRight2,
         answer::kComboTakeable | answer::kStrafing | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::StrafeShootForward1, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootForward2, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootBack1, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootBack2, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootLeft1, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootLeft2, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootRight1, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::StrafeShootRight2, answer::kComboTakeable | answer::kStrafing, 0x21F0071700},
        {Action::FallBack, answer::kFloored | answer::kReacting, 0x0000000000},
        {Action::GetUpBack, answer::kFloored | answer::kReacting, 0x0000000000},
        {Action::FallForward, answer::kFloored | answer::kReacting, 0x0000000000},
        {Action::GetUpForward, answer::kFloored | answer::kReacting, 0x0000000000},
        {Action::SpecialShot,
         answer::kTurboing | answer::kSpecialShooting | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::SpecialShotRecover,
         answer::kTurboing | answer::kSpecialShooting | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::SpikeHit, answer::kReacting, 0x0000000000},
        {Action::Grabbed, answer::kReacting, 0x0000000000},
        {Action::WebReact, answer::kWebbed | answer::kReacting, 0x0000000000},
        {Action::Quick1, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Quick2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Quick3, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Quick2Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Quick3Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::SlowStart,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::SlowSwing,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::SlowRecover,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::LowKick, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::LowKickRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Low1, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::Low2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::LowRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Right, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Right2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::RightRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Right2Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Left, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::Left2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::LeftRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Left2Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Turn, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::Turn2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::TurnRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Turn2Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::TurnLeft, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::TurnLeft2, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::TurnLeftRecover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::TurnLeft2Recover, answer::kComboTakeable | answer::kMeleeing | answer::kIsMelee,
         0x2070000700},
        {Action::Spin,
         answer::kComboTakeable | answer::kMeleeing | answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::SpinRecover,
         answer::kComboTakeable | answer::kMeleeing | answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::Step1,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::Step2,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::Step3,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::Step2Recover,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::Step3Recover,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::WalkStrike,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::WalkStrikeRecover,
         answer::kComboTakeable | answer::kMeleeing | answer::kLunging | answer::kIsMelee,
         0x2070000700},
        {Action::PowerClose,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::PowerCloseRecover,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::PowerMed,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::PowerMedRecover,
         answer::kComboTakeable | answer::kMeleeing | answer::kCanAutoMelee |
             answer::kIsBlockableAttack | answer::kIsMelee,
         0x2070000700},
        {Action::PowerLow, answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::PowerLowRecover, answer::kMeleeing | answer::kIsMelee, 0x2070000700},
        {Action::SpecialShotRepeat,
         answer::kTurboing | answer::kSpecialShooting | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::Hammer,
         answer::kTurboing | answer::kDamageProtected | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::HammerRecover,
         answer::kTurboing | answer::kDamageProtected | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::Breathe,
         answer::kTurboing | answer::kDamageProtected | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::BreatheRecover,
         answer::kTurboing | answer::kDamageProtected | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::FireLeft,
         answer::kComboTakeable | answer::kTurboing | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::FireLeftRecover,
         answer::kComboTakeable | answer::kTurboing | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::FireRight,
         answer::kComboTakeable | answer::kTurboing | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::FireRightRecover,
         answer::kComboTakeable | answer::kTurboing | answer::kItemAttacking |
             answer::kIsBlockableAttack,
         0x2070000300},
        {Action::Pick, answer::kComboTakeable | answer::kCanAutoMelee, 0x21F0071700},
        // Restored at its first frame, a retch lets nothing begin yet.
        {Action::Gag, 0, 0x0000000000},
        {Action::ShieldReady, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::ShieldRun, answer::kComboTakeable | answer::kCanAutoMelee | answer::kLocomotion,
         0x21F0071700},
        {Action::DeathGrabStart, answer::kComboTakeable | answer::kGrabbingDeath, 0x21F0071700},
        {Action::DeathGrab, answer::kComboTakeable | answer::kGrabbingDeath, 0x21F0071700},
        {Action::DeathGrabRelease, answer::kComboTakeable | answer::kGrabbingDeath, 0x21F0071700},
        {Action::Whirled, answer::kFloored | answer::kReacting, 0x0000000000},
        {Action::Pushed, answer::kComboTakeable | answer::kCanAutoMelee, 0x21F0071700},
        {Action::ComboAct1,
         answer::kComboing | answer::kTurboing | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::ComboAct2,
         answer::kComboing | answer::kTurboing | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::ComboAct3,
         answer::kComboing | answer::kTurboing | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::ComboWar1,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboWar2,
         answer::kComboBound | answer::kComboThrown | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::ComboWar3,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboVal,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboWiz,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboArc,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboDwf1,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboDwf2,
         answer::kComboBound | answer::kComboThrown | answer::kDamageProtected |
             answer::kIsBlockableAttack,
         0x0000000000},
        {Action::ComboDwf3,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboKni,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboSor,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
        {Action::ComboJes,
         answer::kComboBound | answer::kDamageProtected | answer::kIsBlockableAttack, 0x0000000000},
    }};
    const auto tree = actionTree();
    const auto bound = boundTo(tree);
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        CAPTURE(i);
        REQUIRE(kRows[i].action == static_cast<Action>(i));
        const auto animator = restoredInto(bound, kRows[i].action);
        const u32 answers = answersOf(animator);
        for (const auto& predicate : kPredicates) {
            CAPTURE(predicate.name);
            CHECK(((answers & predicate.bit) != 0) == ((kRows[i].answers & predicate.bit) != 0));
        }
        const u64 begins = beginsOf(animator);
        for (usize deed = 0; deed < kPlayerDeedCount; ++deed) {
            CAPTURE(deed);
            CHECK(((begins >> deed) & 1U) == ((kRows[i].begins >> deed) & 1U));
        }
    }
    // Past the actions every static answer is no.
    for (const usize beyond : {PlayerAnimator::kActionCount, usize{255}}) {
        CAPTURE(beyond);
        const auto action = static_cast<Action>(beyond);
        CHECK_FALSE(PlayerAnimator::isBlockableAttack(action));
        CHECK_FALSE(PlayerAnimator::isMelee(action));
        CHECK_FALSE(PlayerAnimator::locomotion(action));
        CHECK_FALSE(PlayerAnimator::rollbackTurboAction(action));
        CHECK_FALSE(PlayerAnimator::rollbackShoveAction(action));
    }
}

/** The original's action table (main.dol 0x80126C68): the sequence each of its rows plays. */
constexpr std::array<std::string_view, PlayerAnimator::kNativeActionCount> kNativeSequences{
    "READY",         "IDLE1",         "IDLE2",        "IDLE2_LOOP",    "DEFENDLEFT",
    "DEFENDRIGHT",   "DEFENDBACK",    "DEFENDBACK",   "SHOVE",         "STRAFE_WLKF1",
    "STRAFE_WLKF2",  "STRAFE_WLKB1",  "STRAFE_WLKB2", "STRAFE_WLKL1",  "STRAFE_WLKL2",
    "STRAFE_WLKR1",  "STRAFE_WLKR2",  "WALK1",        "WALK2",         "RUN1",
    "RUN2",          "SHIELD_READY",  "SHIELD_RUN",   "PIVOTL",        "PIVOTR",
    "PUSH",          "PUSHED",        "HITREACT",     "PICK",          "DEATHGRABS",
    "DEATHGRAB",     "DEATHGRABR",    "ATTSTART",     "ATTSLOW1",      "ATTSLOW1R",
    "ATTPWRACLOSE",  "ATTPWRACLOSER", "ATTPWRAMED",   "ATTPWRAMEDR",   "ATTQUICK1",
    "ATTQUICK2",     "ATTQUICK3",     "ATTQUICK2R",   "ATTQUICK3R",    "ATTQ3RIGHT",
    "ATTQ2RIGHT",    "ATTQ3RIGHTR",   "ATTQ2RIGHTR",  "ATTQ3LEFT",     "ATTQ2LEFT",
    "ATTQ3LEFTR",    "ATTQ2LEFTR",    "ATTQ2180",     "ATTQ3180",      "ATTQ2180R",
    "ATTQ3180R",     "ATTQ2180L",     "ATTQ3180L",    "ATTQ2180LR",    "ATTQ3180LR",
    "ATT360",        "ATT360R",       "ATTSTEP1",     "ATTSTEP2",      "ATTSTEP3",
    "ATTSTEP2R",     "ATTSTEP3R",     "ATTQ3TOSTEP1", "ATTQ3TOSTEP1R", "ATTWALK2",
    "ATTWALK2R",     "STRAFE_ATKF1",  "STRAFE_ATKF2", "STRAFE_ATKB1",  "STRAFE_ATKB2",
    "STRAFE_ATKL1",  "STRAFE_ATKL2",  "STRAFE_ATKR1", "STRAFE_ATKR2",  "ATTLOW1",
    "ATTLOW2",       "ATTLOWR",       "ATTLOWK",      "ATTLOWKR",      "ATTPWRALOW",
    "ATTPWRALOWR",   "ATTPWRB",       "ATTPWRC",      "COMBOACT1",     "COMBOACT2",
    "COMBOACT3",     "THROW1S",       "THROW1S",      "THROW2S",       "THROW2S",
    "THROW1",        "THROW2",        "THROW1R",      "THROW2R",       "ATTPWRATHROW",
    "ATTPWRATHROWR", "STRAFE_ATKF1",  "STRAFE_ATKF2", "ATTFIREL",      "ATTFIRELR",
    "ATTFIRER",      "ATTFIRERR",     "SSHOT1",       "SSHOT2",        "SSHOTR",
    "ATTBREATHE",    "ATTBREATHER",   "ATTCHOP",      "ATTCHOPR",      "ATTACK1",
    "MAGICS",        "MAGICR",        "THROWPOTIONS", "THROWPOTIONR",  "DEFEND1",
    "DEFEND2",       "DEFENDR",       "STUN2",        "VICTORY",       "START",
    "INIT",          "DEATH",         "STUN1",        "WEBREACT",      "HITREACT",
    "HITREACT",      "FALLDOWN",      "GETUP",        "FALLFRNT",      "GETUP2",
    "FLYUP",         "COMBOWAR1",     "COMBOWAR2",    "COMBOWAR3",     "COMBOVAL",
    "COMBOWIZ",      "COMBOARC",      "COMBODWF1",    "COMBODWF2",     "COMBODWF3",
    "COMBOKNI",      "COMBOSOR",      "COMBOJES",     "GRABBED"};

TEST_CASE("each action's row of the original's action table names the sequence it plays",
          "[game][players][player-animation][action-traits]") {
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        CAPTURE(i);
        const u8 row = PlayerAnimator::kNativeActions[i];
        REQUIRE(row < kNativeSequences.size());
        CHECK(kNativeSequences[row] == PlayerAnimator::kSequenceNames[i]);
    }
    // Where the table repeats a sequence, the rows PlayerMotion asks for: the plain throw
    // (0x80083964) and its stride's wind-up (DoPlayerAction, 0x800AD114), and the knockback
    // (0x80083AE0), the spikes' being the other.
    CHECK(PlayerAnimator::kNativeActions[static_cast<usize>(Action::Throw)] == 92);
    CHECK(PlayerAnimator::kNativeActions[static_cast<usize>(Action::ThrowMoving)] == 94);
    CHECK(PlayerAnimator::kNativeActions[static_cast<usize>(Action::HitReact)] == 130);
    CHECK(PlayerAnimator::kNativeActions[static_cast<usize>(Action::SpikeHit)] == 129);
}

TEST_CASE("a partner is taken from every action but the original's rows 84 to 90 and from 107, "
          "and not before the entrance",
          "[game][players][player-animation][combo]") {
    // The rows fn_80088EF4 refuses a partner in (0x80089010): ATTPWRALOW to COMBOACT3, and
    // every row from SSHOT1.
    constexpr u8 kFirstRefused = 84;
    constexpr u8 kLastRefused = 90;
    constexpr u8 kRefusedFrom = 107;
    constexpr usize kTakeableActions = 86;
    const auto tree = actionTree();
    const auto bound = boundTo(tree);
    usize takeable = 0;
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        CAPTURE(i);
        const auto action = static_cast<Action>(i);
        const u8 row = PlayerAnimator::kNativeActions[i];
        const bool refused = (row >= kFirstRefused && row <= kLastRefused) || row >= kRefusedFrom;
        CHECK(PlayerAnimator::takeableIn(action) == !refused);
        CHECK(restoredInto(bound, action).comboTakeable() == !refused);
        takeable += refused ? 0 : 1;
    }
    CHECK(takeable == kTakeableActions);
    // A charge, the strong throw and a gauntlet's shot are taken; the gag and the low power
    // swing are not.
    for (const Action action : {Action::Shove, Action::StrongThrow, Action::StrongThrowRecover,
                                Action::FireLeft, Action::FireRightRecover}) {
        CHECK(PlayerAnimator::takeableIn(action));
    }
    CHECK_FALSE(PlayerAnimator::takeableIn(Action::Gag));
    CHECK_FALSE(PlayerAnimator::takeableIn(Action::PowerLow));
    // An entrance yet to play stands for the original's START.
    PlayerAnimator arriving;
    REQUIRE(arriving.bind(tree, Entrance::Play));
    CHECK(arriving.action() == Action::Ready);
    CHECK_FALSE(arriving.comboTakeable());
    for (const usize beyond : {PlayerAnimator::kActionCount, usize{255}}) {
        CHECK_FALSE(PlayerAnimator::takeableIn(static_cast<Action>(beyond)));
    }
}

TEST_CASE("knocks and shoves leave a body where it stands only in the original's attack "
          "categories 11 and 12",
          "[game][players][player-animation][knockback]") {
    // PlayerAttackType (0x800ADBFC) gives its rows 86 and 87 and 107 to 113 category 11, and 88
    // to 90 and 136 to 147 category 12; PlayerMotion moves a body in either by its walk alone
    // (0x80081860-0x80081888).
    constexpr u8 kFirstTurbo = 86;
    constexpr u8 kLastComboMove = 90;
    constexpr u8 kFirstSpecialShot = 107;
    constexpr u8 kLastHammer = 113;
    constexpr u8 kFirstComboBound = 136;
    constexpr u8 kLastComboBound = 147;
    constexpr usize kPlantedActions = 24;
    const auto tree = actionTree();
    const auto bound = boundTo(tree);
    usize planted = 0;
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        CAPTURE(i);
        const auto action = static_cast<Action>(i);
        const u8 row = PlayerAnimator::kNativeActions[i];
        const bool held = (row >= kFirstTurbo && row <= kLastComboMove) ||
                          (row >= kFirstSpecialShot && row <= kLastHammer) ||
                          (row >= kFirstComboBound && row <= kLastComboBound);
        CHECK(PlayerAnimator::plantedIn(action) == held);
        CHECK(restoredInto(bound, action).planted() == held);
        planted += held ? 1 : 0;
    }
    CHECK(planted == kPlantedActions);
    // The charge, the strong throw and a gauntlet's shot are carried; the turbo moves are not.
    for (const Action carried : {Action::Shove, Action::StrongThrow, Action::FireLeft}) {
        CHECK_FALSE(PlayerAnimator::plantedIn(carried));
    }
    for (const Action stays : {Action::TurboStrong, Action::TurboFull, Action::SpecialShot,
                               Action::Breathe, Action::ComboAct1}) {
        CHECK(PlayerAnimator::plantedIn(stays));
    }
    for (const usize beyond : {PlayerAnimator::kActionCount, usize{255}}) {
        CHECK_FALSE(PlayerAnimator::plantedIn(static_cast<Action>(beyond)));
    }
}

TEST_CASE("a retch lets nothing the buttons ask cut in before its tenth frame",
          "[game][players][player-animation][gag]") {
    // DoPlayerAction's STUN2 case answers every request with mode 0 until the retch's tenth
    // frame or the end of its cycle, and with mode 2 after (0x800ACDD8-0x800ACE0C).
    struct Asked {
        PlayerDeed deed;
        Action begins;   ///< what the deed starts once let in
        bool turboClass; ///< whether canBegin answers for it
    };
    constexpr std::array kAsked{
        Asked{PlayerDeed::Attack, Action::Throw, false},
        Asked{PlayerDeed::Melee, Action::Quick1, false},
        Asked{PlayerDeed::UsePotion, Action::UsePotion, false},
        Asked{PlayerDeed::ThrowPotion, Action::ThrowPotion, false},
        Asked{PlayerDeed::Defend, Action::DefendRaise, false},
        Asked{PlayerDeed::TurboStrong, Action::TurboStrong, true},
        Asked{PlayerDeed::Shove, Action::Shove, true},
        Asked{PlayerDeed::Combo, Action::ComboAct1, true},
    };
    constexpr s32 kMostTicks = 120;
    const auto tree = longActionTree();
    const auto bound = boundTo(tree);
    for (const Asked& asked : kAsked) {
        CAPTURE(static_cast<s32>(asked.deed));
        auto animator = bound;
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Gag);
        REQUIRE(animator.action() == Action::Gag);
        std::optional<f32> letIn;
        for (s32 tick = 0; tick < kMostTicks && !letIn; ++tick) {
            const f32 frame = animator.player().frame();
            if (asked.turboClass) {
                CHECK(animator.canBegin(asked.deed) == (frame >= PlayerAnimator::kGagHold));
            }
            animator.update(PlayerMotion::Stand, 1, kStep, asked.deed);
            if (animator.action() != Action::Gag) {
                letIn = frame;
            }
        }
        REQUIRE(letIn.has_value());
        CHECK(animator.action() == asked.begins);
        // Let in on the first update past the hold, from the frame that update began on.
        CHECK(*letIn >= PlayerAnimator::kGagHold);
        CHECK(*letIn < PlayerAnimator::kGagHold + 1.0f);
        CHECK(static_cast<f32>(kLongFrames) > PlayerAnimator::kGagHold + 1.0f);
    }
}

TEST_CASE("each action leaves the body its pace and turn by class, stick and frame",
          "[game][players][player-animation][action-traits]") {
    // For a body of no class at kSpellStormFrame: the pace standing and moving, and the turn.
    struct Scales {
        Action action;
        f32 standing;
        f32 moving;
        f32 turn;
    };
    constexpr std::array<Scales, PlayerAnimator::kActionCount> kScales{{
        {Action::Ready, 1.0f, 1.0f, 1.0f},
        {Action::Idle1, 1.0f, 1.0f, 1.0f},
        {Action::Idle2, 1.0f, 1.0f, 1.0f},
        {Action::Idle2Loop, 1.0f, 1.0f, 1.0f},
        {Action::Walk1, 1.0f, 1.0f, 1.0f},
        {Action::Walk2, 1.0f, 1.0f, 1.0f},
        {Action::Run1, 1.3f, 1.3f, 1.0f},
        {Action::Run2, 1.3f, 1.3f, 1.0f},
        {Action::Start, 0.0f, 0.0f, 1.0f},
        {Action::Throw, 0.0f, 0.0f, 1.0f},
        {Action::ThrowMoving, 0.0f, 0.0f, 1.0f},
        {Action::ThrowRelease, 0.0f, 0.0f, 1.0f},
        {Action::ThrowMovingRelease, 0.0f, 0.0f, 1.0f},
        {Action::ThrowRecover, 0.0f, 0.0f, 1.0f},
        {Action::ThrowMovingRecover, 0.0f, 0.0f, 1.0f},
        {Action::UsePotion, 0.0f, 0.0f, 1.0f},
        {Action::UsePotionRelease, 0.0f, 0.0f, 1.0f},
        {Action::ThrowPotion, 0.0f, 0.0f, 1.0f},
        {Action::ThrowPotionRelease, 0.0f, 0.0f, 1.0f},
        {Action::Death, 1.0f, 1.0f, 1.0f},
        {Action::HitReact, 0.0f, 0.0f, 1.0f},
        {Action::Stun, 0.0f, 0.0f, 1.0f},
        {Action::TurboStrong, 0.0f, 0.0f, 1.0f},
        {Action::TurboFull, 0.0f, 0.0f, 0.25f},
        {Action::Shove, 1.5f, 1.5f, 1.0f},
        {Action::DefendRaise, 0.0f, 0.0f, 1.0f},
        {Action::Defend, 0.0f, 0.0f, 1.0f},
        {Action::DefendLower, 0.0f, 0.0f, 1.0f},
        {Action::StrongThrow, 0.0f, 0.0f, 1.0f},
        {Action::StrongThrowRecover, 0.0f, 0.0f, 1.0f},
        {Action::StrafeForward1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeForward2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeBack1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeBack2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeLeft1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeLeft2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeRight1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeRight2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootForward1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootForward2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootBack1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootBack2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootLeft1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootLeft2, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootRight1, 0.667f, 0.667f, 1.0f},
        {Action::StrafeShootRight2, 0.667f, 0.667f, 1.0f},
        {Action::FallBack, 0.0f, 0.0f, 1.0f},
        {Action::GetUpBack, 0.0f, 0.0f, 1.0f},
        {Action::FallForward, 0.0f, 0.0f, 1.0f},
        {Action::GetUpForward, 0.0f, 0.0f, 1.0f},
        {Action::SpecialShot, 0.0f, 0.0f, 1.0f},
        {Action::SpecialShotRecover, 0.0f, 0.0f, 1.0f},
        {Action::SpikeHit, 0.0f, 0.0f, 1.0f},
        {Action::Grabbed, 0.0f, 0.0f, 1.0f},
        {Action::WebReact, 0.4f, 0.4f, 1.0f},
        {Action::Quick1, 0.25f, 0.25f, 0.0f},
        {Action::Quick2, 0.25f, 0.25f, 0.0f},
        {Action::Quick3, 0.25f, 0.25f, 0.0f},
        {Action::Quick2Recover, 0.25f, 0.25f, 0.0f},
        {Action::Quick3Recover, 0.25f, 0.25f, 0.0f},
        {Action::SlowStart, 0.0f, 0.0f, 1.0f},
        {Action::SlowSwing, 0.0f, 0.0f, 1.0f},
        {Action::SlowRecover, 0.0f, 0.0f, 1.0f},
        {Action::LowKick, 1.0f, 1.0f, 1.0f},
        {Action::LowKickRecover, 1.0f, 1.0f, 1.0f},
        {Action::Low1, 1.0f, 1.0f, 1.0f},
        {Action::Low2, 1.0f, 1.0f, 1.0f},
        {Action::LowRecover, 1.0f, 1.0f, 1.0f},
        {Action::Right, 1.0f, 1.0f, 1.0f},
        {Action::Right2, 1.0f, 1.0f, 1.0f},
        {Action::RightRecover, 1.0f, 1.0f, 1.0f},
        {Action::Right2Recover, 1.0f, 1.0f, 1.0f},
        {Action::Left, 1.0f, 1.0f, 1.0f},
        {Action::Left2, 1.0f, 1.0f, 1.0f},
        {Action::LeftRecover, 1.0f, 1.0f, 1.0f},
        {Action::Left2Recover, 1.0f, 1.0f, 1.0f},
        {Action::Turn, 1.0f, 1.0f, 1.0f},
        {Action::Turn2, 1.0f, 1.0f, 1.0f},
        {Action::TurnRecover, 1.0f, 1.0f, 1.0f},
        {Action::Turn2Recover, 1.0f, 1.0f, 1.0f},
        {Action::TurnLeft, 1.0f, 1.0f, 1.0f},
        {Action::TurnLeft2, 1.0f, 1.0f, 1.0f},
        {Action::TurnLeftRecover, 1.0f, 1.0f, 1.0f},
        {Action::TurnLeft2Recover, 1.0f, 1.0f, 1.0f},
        {Action::Spin, 0.5f, 0.5f, 1.0f},
        {Action::SpinRecover, 0.5f, 0.5f, 1.0f},
        {Action::Step1, 0.5f, 1.0f, 0.25f},
        {Action::Step2, 0.5f, 1.0f, 0.25f},
        {Action::Step3, 0.5f, 1.0f, 0.25f},
        {Action::Step2Recover, 0.5f, 1.0f, 0.25f},
        {Action::Step3Recover, 0.5f, 1.0f, 0.25f},
        {Action::WalkStrike, 0.5f, 1.0f, 0.25f},
        {Action::WalkStrikeRecover, 0.5f, 1.0f, 0.25f},
        {Action::PowerClose, 0.5f, 0.5f, 1.0f},
        {Action::PowerCloseRecover, 0.5f, 0.5f, 1.0f},
        {Action::PowerMed, 0.5f, 0.5f, 1.0f},
        {Action::PowerMedRecover, 0.5f, 0.5f, 1.0f},
        {Action::PowerLow, 0.25f, 0.25f, 1.0f},
        {Action::PowerLowRecover, 0.25f, 0.25f, 1.0f},
        {Action::SpecialShotRepeat, 0.0f, 0.0f, 1.0f},
        {Action::Hammer, 0.0f, 0.0f, 1.0f},
        {Action::HammerRecover, 0.0f, 0.0f, 1.0f},
        {Action::Breathe, 0.0f, 0.0f, 1.0f},
        {Action::BreatheRecover, 0.0f, 0.0f, 1.0f},
        {Action::FireLeft, 0.0f, 0.0f, 1.0f},
        {Action::FireLeftRecover, 0.0f, 0.0f, 1.0f},
        {Action::FireRight, 0.0f, 0.0f, 1.0f},
        {Action::FireRightRecover, 0.0f, 0.0f, 1.0f},
        {Action::Pick, 1.0f, 1.0f, 1.0f},
        {Action::Gag, 1.0f, 1.0f, 1.0f},
        {Action::ShieldReady, 1.0f, 1.0f, 1.0f},
        {Action::ShieldRun, 1.3f, 1.3f, 1.0f},
        {Action::DeathGrabStart, 0.0f, 0.0f, 1.0f},
        {Action::DeathGrab, 0.0f, 0.0f, 1.0f},
        {Action::DeathGrabRelease, 0.0f, 0.0f, 1.0f},
        {Action::Whirled, 0.0f, 0.0f, 1.0f},
        {Action::Pushed, 1.0f, 1.0f, 1.0f},
        {Action::ComboAct1, 0.0f, 0.0f, 0.0f},
        {Action::ComboAct2, 0.0f, 0.0f, 0.0f},
        {Action::ComboAct3, 0.0f, 0.0f, 0.0f},
        {Action::ComboWar1, 0.0f, 0.0f, 0.0f},
        {Action::ComboWar2, 0.0f, 0.0f, 0.0f},
        {Action::ComboWar3, 0.0f, 0.0f, 0.0f},
        {Action::ComboVal, 0.0f, 0.0f, 0.0f},
        {Action::ComboWiz, 0.0f, 0.0f, 0.0f},
        {Action::ComboArc, 0.0f, 0.0f, 0.0f},
        {Action::ComboDwf1, 0.0f, 0.0f, 0.0f},
        {Action::ComboDwf2, 0.0f, 0.0f, 0.5f},
        {Action::ComboDwf3, 0.0f, 0.0f, 0.0f},
        {Action::ComboKni, 0.0f, 0.0f, 0.0f},
        {Action::ComboSor, 0.0f, 0.0f, 0.0f},
        {Action::ComboJes, 0.0f, 0.0f, 0.0f},
    }};
    // The actions whose pace or turn is the class's own, for each starting class: the pace, the
    // turn at kSpellStormFrame and the turn a frame later. Every other action keeps its row above
    // whatever the class.
    struct ClassScales {
        Action action;
        std::array<f32, kStartingClassCount> pace;
        std::array<f32, kStartingClassCount> turn;
        std::array<f32, kStartingClassCount> lateTurn;
    };
    constexpr std::array kClassScales{
        ClassScales{Action::TurboFull,
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                    {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f},
                    {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.0f, 0.25f}},
        ClassScales{Action::PowerClose,
                    {0.5f, 0.5f, 0.25f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f}},
        ClassScales{Action::PowerCloseRecover,
                    {0.5f, 0.5f, 0.25f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f}},
        ClassScales{Action::PowerMed,
                    {0.5f, 0.5f, 0.25f, 0.25f, 0.5f, 0.5f, 0.0f, 0.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f}},
        ClassScales{Action::PowerMedRecover,
                    {0.5f, 0.5f, 0.25f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f}},
    };
    const auto tree = longActionTree();
    const auto bound = boundTo(tree);
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        const auto& row = kScales[i];
        REQUIRE(row.action == static_cast<Action>(i));
        // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto found = std::ranges::find(kClassScales, row.action, &ClassScales::action);
        for (s32 character = -1; character < kStartingClassCount; ++character) {
            for (const bool moved : {false, true}) {
                for (const bool late : {false, true}) {
                    CAPTURE(i, character, moved, late);
                    const auto animator = restoredInto(bound, row.action, character, moved,
                                                       kSpellStormFrame + (late ? 1.0f : 0.0f));
                    f32 pace = moved ? row.moving : row.standing;
                    f32 turn = row.turn;
                    if (found != kClassScales.end() && character >= 0) {
                        const auto column = static_cast<usize>(character);
                        pace = found->pace[column];
                        turn = late ? found->lateTurn[column] : found->turn[column];
                    }
                    CHECK(animator.moveScale() == pace);
                    CHECK(animator.turnScale() == turn);
                }
            }
        }
    }
    // A body that falls dead is left its whole pace: nothing else asks for it.
    CHECK(restoredInto(bound, Action::Death).moveScale() == 1.0f);
}

TEST_CASE("strafing halves, a held grab and the power recoveries keep their own rules",
          "[game][players][player-animation][action-traits]") {
    // No way at all steps forward.
    struct Step {
        StrafeWay way;
        Action walking;
        Action shooting;
    };
    constexpr std::array kSteps{
        Step{StrafeWay::None, Action::StrafeForward1, Action::StrafeShootForward1},
        Step{StrafeWay::Forward, Action::StrafeForward1, Action::StrafeShootForward1},
        Step{StrafeWay::Back, Action::StrafeBack1, Action::StrafeShootBack1},
        Step{StrafeWay::Left, Action::StrafeLeft1, Action::StrafeShootLeft1},
        Step{StrafeWay::Right, Action::StrafeRight1, Action::StrafeShootRight1},
    };
    for (const auto& step : kSteps) {
        CAPTURE(step.way);
        CHECK(PlayerAnimator::strafeStep(step.way, false) == step.walking);
        CHECK(PlayerAnimator::strafeStep(step.way, true) == step.shooting);
    }
    struct Halves {
        Action step;
        Action first;
        Action other;
    };
    constexpr std::array kHalves{
        Halves{Action::StrafeForward1, Action::StrafeForward1, Action::StrafeForward2},
        Halves{Action::StrafeForward2, Action::StrafeForward1, Action::StrafeForward1},
        Halves{Action::StrafeBack1, Action::StrafeBack1, Action::StrafeBack2},
        Halves{Action::StrafeBack2, Action::StrafeBack1, Action::StrafeBack1},
        Halves{Action::StrafeLeft1, Action::StrafeLeft1, Action::StrafeLeft2},
        Halves{Action::StrafeLeft2, Action::StrafeLeft1, Action::StrafeLeft1},
        Halves{Action::StrafeRight1, Action::StrafeRight1, Action::StrafeRight2},
        Halves{Action::StrafeRight2, Action::StrafeRight1, Action::StrafeRight1},
        Halves{Action::StrafeShootForward1, Action::StrafeShootForward1,
               Action::StrafeShootForward2},
        Halves{Action::StrafeShootForward2, Action::StrafeShootForward1,
               Action::StrafeShootForward1},
        Halves{Action::StrafeShootBack1, Action::StrafeShootBack1, Action::StrafeShootBack2},
        Halves{Action::StrafeShootBack2, Action::StrafeShootBack1, Action::StrafeShootBack1},
        Halves{Action::StrafeShootLeft1, Action::StrafeShootLeft1, Action::StrafeShootLeft2},
        Halves{Action::StrafeShootLeft2, Action::StrafeShootLeft1, Action::StrafeShootLeft1},
        Halves{Action::StrafeShootRight1, Action::StrafeShootRight1, Action::StrafeShootRight2},
        Halves{Action::StrafeShootRight2, Action::StrafeShootRight1, Action::StrafeShootRight1},
    };
    for (const auto& halves : kHalves) {
        CAPTURE(halves.step);
        CHECK(PlayerAnimator::firstHalfOf(halves.step) == halves.first);
        CHECK(PlayerAnimator::otherHalfOf(halves.step) == halves.other);
    }

    const auto tree = actionTree();
    const auto bound = boundTo(tree);
    // A swing ending into its recovery with a quick press pending: the power recoveries spend it.
    struct Recovery {
        Action swing;
        Action recovery;
        bool keepsPress;
    };
    constexpr std::array kRecoveries{
        Recovery{Action::PowerClose, Action::PowerCloseRecover, false},
        Recovery{Action::PowerMed, Action::PowerMedRecover, false},
        Recovery{Action::PowerLow, Action::PowerLowRecover, false},
        Recovery{Action::Spin, Action::SpinRecover, true},
        Recovery{Action::SlowSwing, Action::SlowRecover, true},
    };
    for (const auto& recovery : kRecoveries) {
        CAPTURE(recovery.swing);
        auto animator = restoredInto(bound, recovery.swing, -1, false, 0.0f, true);
        for (s32 tick = 0; tick < 60 && animator.action() == recovery.swing; ++tick) {
            animator.update(PlayerMotion::Stand, 1, kStep);
        }
        REQUIRE(animator.action() == recovery.recovery);
        CHECK(animator.checkpoint().quickPress == recovery.keepsPress);
    }

    // Presentation alone runs a grab to its end and holds it there, where a looping fidget
    // starts over.
    auto grabbed = restoredInto(bound, Action::Grabbed);
    const auto held = grabbed.player().generation();
    grabbed.advancePresentation(1.0f);
    CHECK(grabbed.action() == Action::Grabbed);
    CHECK(grabbed.player().generation() == held);
    CHECK(grabbed.player().checkpoint().held);
    auto fidget = restoredInto(bound, Action::Idle2Loop);
    const auto looped = fidget.player().generation();
    fidget.advancePresentation(1.0f);
    CHECK(fidget.action() == Action::Idle2Loop);
    CHECK(fidget.player().generation() != looped);
    CHECK_FALSE(fidget.player().checkpoint().held);
}

TEST_CASE("each action keeps what it does over every input script for every class tried",
          "[game][players][player-animation][action-trace]") {
    // traceDigest of each script: what every action does under it, which a change to how the
    // animator reads its actions must keep.
    constexpr std::array kScripts{
        TraceScript{"standing", standing,
                    "af0bc552f6ca12db80839fe9283680ac85fb3d049ad83190240a85ef809d9e14"},
        TraceScript{"afoot", afoot,
                    "154feac6822b91c3b4ed69b07182769412e1acbf0f8ebf9bbae03e081635aa75"},
        TraceScript{"throwing", throwing,
                    "1f5053fca6c7f2670c6718e02d48697e3d8bfc97b4f2f26847c675d730a92b60"},
        TraceScript{"quickPresses", quickPresses,
                    "ea779c33ab0391ae970996d4acf10bc2efd664252d1fb0e255dbed30eb720483"},
        TraceScript{"steppingSwings", steppingSwings,
                    "b1bbf212667b54a45d237c42964c07510f637ecca7bf3a31379197ecd324974e"},
        TraceScript{"lowStrikes", lowStrikes,
                    "b8e806d2d2ec0662b66cbc3ee7fe685ccd4c6aa31b2b1485fd8aed566067a085"},
        TraceScript{"heldChains", heldChains,
                    "dc94862a665a4a403c1511654bfd6786568265212904156282c82aa834b36689"},
        TraceScript{"automaticMelee", automaticMelee,
                    "610911a098384ea98b498bbb8592c4c614c7cd810173e649397d2d9365b2f98b"},
        TraceScript{"everyDeed", everyDeed,
                    "9c97598a5b36cea1f158db72646e30e6b5ff515b023212e423c507b1b94dc136"},
        TraceScript{"heldDeeds", heldDeeds,
                    "f75890a814bbd3893882eda624b4b4a90088ae22f0c56b53526aa91b707bcbff"},
        TraceScript{"strafing", strafing,
                    "bbe4ad40632572cb0267790b3a8b5f8496efdbaf2056109a169fdb7c2c7255fd"},
        TraceScript{"combos", combos,
                    "754cc13ffe51c9ee5c1abfbd1d170c4c1df14802c4cbbd303d9db2f1ce841322"},
        TraceScript{"falling", falling,
                    "ba95d6e9882c5cd47cf97a09e52ee67060e561ef6b12442fb33c00c330500f0f"},
    };
    const auto tree = actionTree();
    const auto bound = boundTo(tree);
    for (const auto& script : kScripts) {
        CAPTURE(script.name);
        CHECK(traceDigest(bound, script.input) == script.digest);
    }
}

} // namespace
