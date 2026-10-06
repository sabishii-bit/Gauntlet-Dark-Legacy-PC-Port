#include <array>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Action = PlayerAnimator::Action;

TreeInfo actionTree() {
    TreeInfo tree;
    tree.name = "ACTIONS";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 12;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

struct OrdinaryAction {
    PlayerDeed request;
    Action phase;
};

constexpr std::array kOrdinaryActions{
    OrdinaryAction{PlayerDeed::Attack, Action::Throw},
    OrdinaryAction{PlayerDeed::Attack, Action::ThrowRelease},
    OrdinaryAction{PlayerDeed::Attack, Action::ThrowRecover},
    OrdinaryAction{PlayerDeed::StrongAttack, Action::StrongThrow},
    OrdinaryAction{PlayerDeed::StrongAttack, Action::StrongThrowRecover},
    OrdinaryAction{PlayerDeed::Melee, Action::Quick1},
    OrdinaryAction{PlayerDeed::Melee, Action::Quick3Recover},
    OrdinaryAction{PlayerDeed::FireLeft, Action::FireLeft},
    OrdinaryAction{PlayerDeed::FireLeft, Action::FireLeftRecover},
    OrdinaryAction{PlayerDeed::FireRight, Action::FireRight},
    OrdinaryAction{PlayerDeed::FireRight, Action::FireRightRecover},
    OrdinaryAction{PlayerDeed::Defend, Action::DefendRaise},
    OrdinaryAction{PlayerDeed::Shove, Action::Shove}};

void reach(PlayerAnimator& animator, const OrdinaryAction& source, s32 hz) {
    const f32 seconds = 1.0f / static_cast<f32>(hz);
    animator.update(PlayerMotion::Stand, 60 / hz, seconds, source.request);
    for (s32 tick = 0; tick < hz * 3 && animator.action() != source.phase; ++tick) {
        animator.update(PlayerMotion::Stand, 60 / hz, seconds);
    }
    REQUIRE(animator.action() == source.phase);
}

TEST_CASE("turbo and special attacks interrupt ordinary attack phases immediately",
          "[game][players][animation][attack-interrupt]") {
    // DoPlayerAction (800AC068) dispatches current categories 1..10 through
    // READY with mode 2 when the new attack category is >=11. The controller
    // selector (80088938) does not discard that press during a throw.
    const auto tree = actionTree();
    for (const s32 hz : {30, 60}) {
        for (const auto& source : kOrdinaryActions) {
            for (const auto request :
                 {PlayerDeed::TurboStrong, PlayerDeed::TurboFull, PlayerDeed::SuperShot,
                  PlayerDeed::Hammer, PlayerDeed::Breathe, PlayerDeed::Combo}) {
                CAPTURE(hz, source.phase, request);
                PlayerAnimator animator;
                REQUIRE(animator.bind(tree, false));
                reach(animator, source, hz);
                CHECK(animator.canBegin(request));
                animator.update(PlayerMotion::Stand, 60 / hz, 1.0f / static_cast<f32>(hz), request);
                REQUIRE(animator.action() == PlayerAnimator::turboActionOf(request));
                CHECK(animator.player().frame() == 0.0f);
                CHECK(animator.damageProtected());
                CHECK_FALSE(animator.potionUsed());
                CHECK_FALSE(animator.potionThrown());
                CHECK_FALSE(animator.superReleased());
                CHECK_FALSE(animator.canBegin(PlayerDeed::TurboFull));
            }
        }
    }
}

TEST_CASE("potion requests replace ordinary attacks and emit their magic only once",
          "[game][players][animation][attack-interrupt]") {
    // Native requests at or after P_USE_MAGIC reset categories 1..10 to
    // READY before dispatch, including a throw wind-up, release or recovery.
    const auto tree = actionTree();
    for (const s32 hz : {30, 60}) {
        for (const auto& source : kOrdinaryActions) {
            for (const auto request :
                 {PlayerDeed::UsePotion, PlayerDeed::ThrowPotion, PlayerDeed::ShieldPotion}) {
                CAPTURE(hz, source.phase, request);
                PlayerAnimator animator;
                REQUIRE(animator.bind(tree, false));
                reach(animator, source, hz);
                const f32 seconds = 1.0f / static_cast<f32>(hz);
                animator.update(PlayerMotion::Stand, 60 / hz, seconds, request);
                REQUIRE(
                    animator.action() ==
                    (request == PlayerDeed::ThrowPotion ? Action::ThrowPotion : Action::UsePotion));
                CHECK(animator.player().frame() == 0.0f);
                CHECK_FALSE(animator.potionUsed());
                CHECK_FALSE(animator.potionThrown());
                CHECK_FALSE(animator.potionShielded());
                s32 used = 0;
                s32 thrown = 0;
                s32 shielded = 0;
                for (s32 tick = 0; tick < hz * 3; ++tick) {
                    animator.update(PlayerMotion::Stand, 60 / hz, seconds, request);
                    used += animator.potionUsed() ? 1 : 0;
                    thrown += animator.potionThrown() ? 1 : 0;
                    shielded += animator.potionShielded() ? 1 : 0;
                }
                CHECK(used == (request == PlayerDeed::UsePotion ? 1 : 0));
                CHECK(thrown == (request == PlayerDeed::ThrowPotion ? 1 : 0));
                CHECK(shielded == (request == PlayerDeed::ShieldPotion ? 1 : 0));
                CHECK(animator.action() == Action::Ready);
            }
        }
    }
}

TEST_CASE("priority attacks do not bypass special moves reactions or entrance locks",
          "[game][players][animation][attack-interrupt]") {
    const auto tree = actionTree();
    for (const auto source :
         {PlayerDeed::TurboStrong, PlayerDeed::TurboFull, PlayerDeed::SuperShot, PlayerDeed::Hammer,
          PlayerDeed::Breathe, PlayerDeed::Combo, PlayerDeed::UsePotion, PlayerDeed::ThrowPotion,
          PlayerDeed::Flinch, PlayerDeed::FallBack, PlayerDeed::Die, PlayerDeed::HurlLegend,
          PlayerDeed::ThrowLegend, PlayerDeed::ShootLegend, PlayerDeed::None}) {
        for (const auto request : {PlayerDeed::TurboFull, PlayerDeed::UsePotion}) {
            CAPTURE(source, request);
            PlayerAnimator animator;
            REQUIRE(animator.bind(tree, source == PlayerDeed::None));
            animator.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, source);
            const auto phase = animator.action();
            const auto generation = animator.player().generation();
            CHECK_FALSE(animator.canBegin(PlayerDeed::TurboFull));
            animator.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, request);
            CHECK(animator.action() == phase);
            CHECK(animator.player().generation() == generation);
            CHECK_FALSE(animator.turboBegan());
            CHECK_FALSE(animator.potionUsed());
        }
    }
}

TEST_CASE("all eight native class trees accept magic or turbo during a throw windup",
          "[game][players][animation][attack-interrupt][assets]") {
    s32 character = 0;
    for (const auto* code : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        const auto path = test::assetOrSkip(std::format("PLAYERS/{}/ANIM/ANIM.PS2", code));
        AnimationSet actions;
        REQUIRE(actions.load(path.parent_path()));
        const auto found = actions.find(code);
        REQUIRE(found);
        const auto& tree = actions.tree(*found);
        for (const auto source : {PlayerDeed::Attack, PlayerDeed::StrongAttack}) {
            for (const auto request : {PlayerDeed::UsePotion, PlayerDeed::TurboFull}) {
                CAPTURE(code, source, request);
                PlayerAnimator animator;
                REQUIRE(animator.bind(tree, false));
                animator.setCharacter(character);
                animator.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, source);
                REQUIRE(animator.action() ==
                        (source == PlayerDeed::Attack ? Action::Throw : Action::StrongThrow));
                animator.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, request);
                const auto expected =
                    request == PlayerDeed::UsePotion ? Action::UsePotion : Action::TurboFull;
                REQUIRE(animator.action() == expected);
                CHECK(tree.sequences[animator.player().sequence()].name ==
                      (request == PlayerDeed::UsePotion ? "MAGICS" : "ATTPWRC"));
                CHECK_FALSE(animator.released());
                CHECK_FALSE(animator.strongReleased());
            }
        }
        ++character;
    }
}

} // namespace
