#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"

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

} // namespace
