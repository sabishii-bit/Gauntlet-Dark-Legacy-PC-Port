#include "game/players/PlayerAnimator.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/characters/ClassData.h"

namespace gdl::game {

void visitNativeState(NativeStateDigest& digest, const PlayerAnimator& value) {
    const auto& [tree, sequences, current, footfall, entered, released, meleeBlow, melee, character,
                 chain, quickPress, strongPress, quickHeld, strongHeld, moved, shielded, pushed,
                 potionUsed, potionThrown, potionThrowTicks, potionThrowReleased, dead, turboBegan,
                 strongReleased, potionShielded, shieldAsked, legendAsked, legendReleased,
                 superReleased, superHeld, itemReleased, rapid, speed, guardArmor, strafe,
                 comboClass, comboRide, potionLatch, attackSeconds, phaseSeconds, shotFrame,
                 stillTicks, fidgetTicks, player, pose, previous, presentationPrevious,
                 presentationGeneration] = value;
    digest.resource(tree);
    digest.values(sequences, current, footfall, entered, released, meleeBlow);
    digest.fields<3>(melee);
    digest.values(character, chain, quickPress, strongPress, quickHeld, strongHeld, moved, shielded,
                  pushed, potionUsed, potionThrown, potionThrowTicks, potionThrowReleased, dead,
                  turboBegan, strongReleased, potionShielded, shieldAsked, legendAsked,
                  legendReleased, superReleased, superHeld, itemReleased, rapid, speed, guardArmor,
                  strafe, comboClass, comboRide, potionLatch, attackSeconds, phaseSeconds,
                  shotFrame, stillTicks, fidgetTicks, player, pose, previous, presentationPrevious,
                  presentationGeneration);
}

namespace {

constexpr usize index(PlayerAnimator::Action action) {
    return static_cast<usize>(action);
}

// AnimAction's refinement of a swing by the way to its target against the facing.
constexpr f32 kBehind = 3.0f * std::numbers::pi_v<f32> / 4.0f;
constexpr f32 kAside = std::numbers::pi_v<f32> / 3.0f;

// How much of its pace and turn a close attack leaves the body (AnimAction's move and turn
// scales by action).
constexpr f32 kPowerPace = 0.5f;
constexpr f32 kWizardPowerPace = 0.25f;
constexpr f32 kSpinPace = 0.5f;
constexpr f32 kIdleStepPace = 0.5f;
constexpr f32 kPowerLowPace = 0.25f;
constexpr f32 kStepTurn = 0.25f;
constexpr f32 kChargerTurn = 0.5f; // COMBODWF2's turn scale (action.c 1804)
constexpr f32 kFullTurboTurn = 0.25f;
constexpr f32 kSpellStormLockFrame = 11;

/** Which run of actions an action belongs to; the predicates over the current action are
 * unions of these. */
enum class ActionFamily : u8 {
    Stance,
    Fidget,
    Walk,
    Run,
    Entrance,
    Throw,
    Potion,
    Death,
    Reaction, ///< a blow, a stun, spikes, a grab or a web
    Turbo,
    Shove,
    Guard,
    StrongThrow,
    StrafeWalk,
    StrafeShoot,
    Floored, ///< off its feet or getting up, a whirlwind's too
    SpecialShot,
    Melee,
    Item, ///< the hammer, the breath and the gauntlets
    Pick,
    Gag,
    Shield,
    DeathGrab,
    Pushed,
    ComboAct,
    ComboBound,
    None ///< outside the actions
};

/** DoPlayerAction's attack category (PlayerAttackType, 0x800ADBFC), numbered as the original. */
enum class AttackType : u8 {
    None,
    Guard, ///< the guard and the charge
    Slow,  ///< the slow swing and the close and middle power swings
    Quick,
    Turned,
    Spin,
    Step,
    StrafeShot,
    Low, ///< the low strikes, the kick and the low power swing
    Throw,
    StrongThrow, ///< the strong throw and the gauntlets
    Special,     ///< the turbo moves, the special shot, the hammer and the breath
    Combo
};

/** Whether a category is one of DoPlayerAction's ordinary attacks, 1 to 10. */
constexpr bool ordinaryAttackType(AttackType type) {
    return type != AttackType::None && type != AttackType::Special && type != AttackType::Combo;
}

/** The pace and turn rules that depend on class, motion or frame; the rest are fixed values. */
enum class MotionRule : u8 {
    Fixed,
    Step,       ///< the row's pace while the stick moves, else kIdleStepPace
    PowerClose, ///< the class's close power move (PowerMoves)
    PowerMed,   ///< the class's middle power move
    SpellStorm  ///< the sorceress's full turbo stops turning past kSpellStormLockFrame
};

namespace action_trait {
constexpr u8 kBlendsToReady = 0x1; ///< eases back into the stance as it ends (readyTransition)
constexpr u8 kAutoMelee = 0x2;     ///< an unpressed contact swing may begin from it
constexpr u8 kKeepsPress = 0x4;    ///< keeps a pending attack press for the next request
constexpr u8 kShotBoundary = 0x8;  ///< what follows it waits for a nominal shot boundary
} // namespace action_trait

/** Per class: whether the close and middle power moves root it, and at which rate it moves. */
struct PowerMoves {
    s32 character;
    bool rootedClose = false;
    bool rootedMed = false;
    f32 rate = kPowerPace;
    f32 medRate = kPowerPace;
};

constexpr std::array<PowerMoves, static_cast<usize>(kStartingClassCount)> kPowerMoves{{
    {.character = kWarriorClass},
    {.character = kValkyrieClass},
    {.character = kWizardClass, .rate = kWizardPowerPace, .medRate = kWizardPowerPace},
    {.character = kArcherClass, .medRate = kWizardPowerPace},
    {.character = kDwarfClass},
    {.character = kKnightClass, .rootedClose = true},
    {.character = kSorceressClass, .rootedClose = true, .rootedMed = true},
    {.character = kJesterClass, .rootedMed = true},
}};
static_assert(
    [] {
        for (usize i = 0; i < kPowerMoves.size(); ++i) {
            if (kPowerMoves[i].character != static_cast<s32>(i)) {
                return false;
            }
        }
        return true;
    }(),
    "each class's power moves sit at its index's row");

/** A body of no starting class: the ordinary rates, rooted by neither move. */
constexpr PowerMoves kNoClassPowerMoves{.character = -1};

const PowerMoves& powerMovesOf(s32 character) {
    if (character < 0 || character >= kStartingClassCount) {
        return kNoClassPowerMoves;
    }
    return kPowerMoves[static_cast<usize>(character)];
}

} // namespace

struct PlayerAnimator::ActionInfo {
    Action action;
    ActionFamily family;
    AttackType attackType = AttackType::None;
    f32 pace = 1.0f; ///< of the body's pace it leaves, by its motion rule (moveScale)
    f32 turn = 1.0f; ///< of the turn toward the stick it leaves, by its motion rule (turnScale)
    MotionRule motion = MotionRule::Fixed;
    MeleeBlow blow = MeleeBlow::None; ///< what the swing does to what it strikes as it completes
    /** What it gives way to by itself as it ends (its release, swing, recovery or get-up);
     * Ready: none of its own, whatever is asked follows. */
    Action next = Action::Ready;
    Cut cut = Cut::WhenDoneIfDifferent; ///< when what follows it may start
    Foot foot = Foot::None;             ///< the foot that comes down as its half cycle ends
    Action otherHalf = Action::Ready;   ///< a half cycle's other half (Ready: none)
    u8 traits = 0;                      ///< action_trait bits
};

const PlayerAnimator::ActionInfo& PlayerAnimator::info(Action action) {
    using action_trait::kAutoMelee;
    using action_trait::kBlendsToReady;
    using action_trait::kKeepsPress;
    using action_trait::kShotBoundary;
    // By action: AnimAction's move and turn scales, its blends back to the stance, what each
    // action gives way to as it ends and the feet and other halves of the walking, running
    // and strafing half cycles, and DoPlayerAction's attack categories and press latches. A
    // body in a partner's combo goes where the combo takes it, and the charge rushes on,
    // faster than a run. What follows the stance, the fidgets, the chains of close attacks,
    // the throw's wind-up, the raised guard, the special shot, the combo move's opening and
    // the loops held while asked for turns on more than the action, and continuation says so
    // itself.
    static constexpr std::array<ActionInfo, kActionCount> kActions{{
        {.action = Action::Ready, .family = ActionFamily::Stance, .traits = kAutoMelee},
        {.action = Action::Idle1,
         .family = ActionFamily::Fidget,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Idle2,
         .family = ActionFamily::Fidget,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Idle2Loop,
         .family = ActionFamily::Fidget,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Walk1,
         .family = ActionFamily::Walk,
         .foot = Foot::First,
         .otherHalf = Action::Walk2,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Walk2,
         .family = ActionFamily::Walk,
         .foot = Foot::Second,
         .otherHalf = Action::Walk1,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Run1,
         .family = ActionFamily::Run,
         .pace = kRunPace,
         .foot = Foot::First,
         .otherHalf = Action::Run2,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Run2,
         .family = ActionFamily::Run,
         .pace = kRunPace,
         .foot = Foot::Second,
         .otherHalf = Action::Run1,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Start, .family = ActionFamily::Entrance, .pace = 0.0f},
        {.action = Action::Throw,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f},
        {.action = Action::ThrowMoving,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f},
        {.action = Action::ThrowRelease,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f,
         .next = Action::ThrowRecover,
         .traits = kShotBoundary},
        {.action = Action::ThrowMovingRelease,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f,
         .next = Action::ThrowMovingRecover,
         .traits = kShotBoundary},
        {.action = Action::ThrowRecover,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f,
         .traits = kKeepsPress | kShotBoundary},
        {.action = Action::ThrowMovingRecover,
         .family = ActionFamily::Throw,
         .attackType = AttackType::Throw,
         .pace = 0.0f,
         .traits = kKeepsPress | kShotBoundary},
        {.action = Action::UsePotion,
         .family = ActionFamily::Potion,
         .pace = 0.0f,
         .next = Action::UsePotionRelease,
         .traits = kAutoMelee},
        {.action = Action::UsePotionRelease,
         .family = ActionFamily::Potion,
         .pace = 0.0f,
         .traits = kAutoMelee},
        {.action = Action::ThrowPotion,
         .family = ActionFamily::Potion,
         .pace = 0.0f,
         .next = Action::ThrowPotionRelease,
         .traits = kAutoMelee},
        {.action = Action::ThrowPotionRelease,
         .family = ActionFamily::Potion,
         .pace = 0.0f,
         .traits = kAutoMelee},
        {.action = Action::Death, .family = ActionFamily::Death, .traits = kKeepsPress},
        {.action = Action::HitReact, .family = ActionFamily::Reaction, .pace = 0.0f},
        {.action = Action::Stun, .family = ActionFamily::Reaction, .pace = 0.0f},
        {.action = Action::TurboStrong,
         .family = ActionFamily::Turbo,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::TurboFull,
         .family = ActionFamily::Turbo,
         .attackType = AttackType::Special,
         .pace = 0.0f,
         .turn = kFullTurboTurn,
         .motion = MotionRule::SpellStorm},
        {.action = Action::Shove,
         .family = ActionFamily::Shove,
         .attackType = AttackType::Guard,
         .pace = kChargePace,
         .traits = kBlendsToReady},
        {.action = Action::DefendRaise,
         .family = ActionFamily::Guard,
         .attackType = AttackType::Guard,
         .pace = 0.0f,
         .next = Action::Defend,
         .cut = Cut::WhenDone,
         .traits = kKeepsPress},
        {.action = Action::Defend,
         .family = ActionFamily::Guard,
         .attackType = AttackType::Guard,
         .pace = 0.0f,
         .traits = kKeepsPress},
        {.action = Action::DefendLower,
         .family = ActionFamily::Guard,
         .attackType = AttackType::Guard,
         .pace = 0.0f},
        // The weapon leaves as the wind-up ends.
        {.action = Action::StrongThrow,
         .family = ActionFamily::StrongThrow,
         .attackType = AttackType::StrongThrow,
         .pace = kStrongThrowPace,
         .next = Action::StrongThrowRecover},
        {.action = Action::StrongThrowRecover,
         .family = ActionFamily::StrongThrow,
         .attackType = AttackType::StrongThrow,
         .pace = kStrongThrowPace,
         .traits = kKeepsPress},
        {.action = Action::StrafeForward1,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeForward2,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeForward2,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeForward1,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeBack1,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeBack2,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeBack2,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeBack1,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeLeft1,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeLeft2,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeLeft2,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeLeft1,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeRight1,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeRight2,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeRight2,
         .family = ActionFamily::StrafeWalk,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeRight1,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::StrafeShootForward1,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeShootForward2,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootForward2,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeShootForward1,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootBack1,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeShootBack2,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootBack2,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeShootBack1,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootLeft1,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeShootLeft2,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootLeft2,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeShootLeft1,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootRight1,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::First,
         .otherHalf = Action::StrafeShootRight2,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::StrafeShootRight2,
         .family = ActionFamily::StrafeShoot,
         .attackType = AttackType::StrafeShot,
         .pace = kStrafePace,
         .foot = Foot::Second,
         .otherHalf = Action::StrafeShootRight1,
         .traits = kBlendsToReady | kShotBoundary},
        {.action = Action::FallBack,
         .family = ActionFamily::Floored,
         .pace = 0.0f,
         .next = Action::GetUpBack,
         .traits = kKeepsPress},
        {.action = Action::GetUpBack,
         .family = ActionFamily::Floored,
         .pace = 0.0f,
         .traits = kKeepsPress},
        {.action = Action::FallForward,
         .family = ActionFamily::Floored,
         .pace = 0.0f,
         .next = Action::GetUpForward,
         .traits = kKeepsPress},
        {.action = Action::GetUpForward,
         .family = ActionFamily::Floored,
         .pace = 0.0f,
         .traits = kKeepsPress},
        {.action = Action::SpecialShot,
         .family = ActionFamily::SpecialShot,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::SpecialShotRecover,
         .family = ActionFamily::SpecialShot,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::SpikeHit, .family = ActionFamily::Reaction, .pace = 0.0f},
        {.action = Action::Grabbed,
         .family = ActionFamily::Reaction,
         .pace = 0.0f,
         .traits = kBlendsToReady},
        {.action = Action::WebReact, .family = ActionFamily::Reaction, .pace = kWebPace},
        {.action = Action::Quick1,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Quick,
         .pace = kQuickMeleePace,
         .turn = 0.0f,
         .blow = MeleeBlow::Plain,
         .next = Action::Quick3Recover,
         .traits = kBlendsToReady},
        {.action = Action::Quick2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Quick,
         .pace = kQuickMeleePace,
         .turn = 0.0f,
         .blow = MeleeBlow::Plain,
         .next = Action::Quick2Recover,
         .traits = kBlendsToReady},
        {.action = Action::Quick3,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Quick,
         .pace = kQuickMeleePace,
         .turn = 0.0f,
         .blow = MeleeBlow::Plain,
         .next = Action::Quick3Recover,
         .traits = kBlendsToReady},
        {.action = Action::Quick2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Quick,
         .pace = kQuickMeleePace,
         .turn = 0.0f,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Quick3Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Quick,
         .pace = kQuickMeleePace,
         .turn = 0.0f,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::SlowStart,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .pace = 0.0f,
         .next = Action::SlowSwing,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::SlowSwing,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .pace = 0.0f,
         .blow = MeleeBlow::Heavy,
         .next = Action::SlowRecover,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::SlowRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .pace = 0.0f,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kAutoMelee | kKeepsPress},
        {.action = Action::LowKick,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .blow = MeleeBlow::Kick,
         .next = Action::LowKickRecover,
         .traits = kBlendsToReady},
        {.action = Action::LowKickRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Low1,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .blow = MeleeBlow::Plain,
         .next = Action::LowRecover,
         .traits = kBlendsToReady},
        {.action = Action::Low2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .blow = MeleeBlow::Plain,
         .next = Action::LowRecover,
         .traits = kBlendsToReady},
        {.action = Action::LowRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Right,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::RightRecover,
         .traits = kBlendsToReady},
        {.action = Action::Right2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::Right2Recover,
         .traits = kBlendsToReady},
        {.action = Action::RightRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Right2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Left,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::LeftRecover,
         .traits = kBlendsToReady},
        {.action = Action::Left2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::Left2Recover,
         .traits = kBlendsToReady},
        {.action = Action::LeftRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Left2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Turn,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::TurnRecover,
         .traits = kBlendsToReady},
        {.action = Action::Turn2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::Turn2Recover,
         .traits = kBlendsToReady},
        {.action = Action::TurnRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Turn2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::TurnLeft,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::TurnLeftRecover,
         .traits = kBlendsToReady},
        {.action = Action::TurnLeft2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .blow = MeleeBlow::Plain,
         .next = Action::TurnLeft2Recover,
         .traits = kBlendsToReady},
        {.action = Action::TurnLeftRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::TurnLeft2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Turned,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Spin,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Spin,
         .pace = kSpinPace,
         .blow = MeleeBlow::Plain,
         .next = Action::SpinRecover,
         .traits = kBlendsToReady},
        {.action = Action::SpinRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Spin,
         .pace = kSpinPace,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Step1,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .blow = MeleeBlow::Heavy,
         .next = Action::Step3Recover,
         .traits = kBlendsToReady},
        {.action = Action::Step2,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .blow = MeleeBlow::Heavy,
         .next = Action::Step2Recover,
         .traits = kBlendsToReady},
        {.action = Action::Step3,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .blow = MeleeBlow::Heavy,
         .next = Action::Step3Recover,
         .traits = kBlendsToReady},
        {.action = Action::Step2Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::Step3Recover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::WalkStrike,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .blow = MeleeBlow::Heavy,
         .next = Action::WalkStrikeRecover,
         .traits = kBlendsToReady},
        {.action = Action::WalkStrikeRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Step,
         .turn = kStepTurn,
         .motion = MotionRule::Step,
         .traits = kBlendsToReady | kKeepsPress},
        {.action = Action::PowerClose,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .motion = MotionRule::PowerClose,
         .blow = MeleeBlow::Power,
         .next = Action::PowerCloseRecover,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::PowerCloseRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .motion = MotionRule::PowerClose,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::PowerMed,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .motion = MotionRule::PowerMed,
         .blow = MeleeBlow::Power,
         .next = Action::PowerMedRecover,
         .traits = kBlendsToReady | kAutoMelee},
        // The middle power swing recovers by the close one's rule.
        {.action = Action::PowerMedRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Slow,
         .motion = MotionRule::PowerClose,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::PowerLow,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .pace = kPowerLowPace,
         .blow = MeleeBlow::Power,
         .next = Action::PowerLowRecover,
         .traits = kBlendsToReady},
        {.action = Action::PowerLowRecover,
         .family = ActionFamily::Melee,
         .attackType = AttackType::Low,
         .pace = kPowerLowPace,
         .cut = Cut::WhenDone,
         .traits = kBlendsToReady},
        {.action = Action::SpecialShotRepeat,
         .family = ActionFamily::SpecialShot,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::Hammer,
         .family = ActionFamily::Item,
         .attackType = AttackType::Special,
         .pace = 0.0f,
         .next = Action::HammerRecover},
        {.action = Action::HammerRecover,
         .family = ActionFamily::Item,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::Breathe,
         .family = ActionFamily::Item,
         .attackType = AttackType::Special,
         .pace = 0.0f,
         .next = Action::BreatheRecover},
        {.action = Action::BreatheRecover,
         .family = ActionFamily::Item,
         .attackType = AttackType::Special,
         .pace = 0.0f},
        {.action = Action::FireLeft,
         .family = ActionFamily::Item,
         .attackType = AttackType::StrongThrow,
         .pace = 0.0f,
         .next = Action::FireLeftRecover},
        {.action = Action::FireLeftRecover,
         .family = ActionFamily::Item,
         .attackType = AttackType::StrongThrow,
         .pace = 0.0f},
        {.action = Action::FireRight,
         .family = ActionFamily::Item,
         .attackType = AttackType::StrongThrow,
         .pace = 0.0f,
         .next = Action::FireRightRecover},
        {.action = Action::FireRightRecover,
         .family = ActionFamily::Item,
         .attackType = AttackType::StrongThrow,
         .pace = 0.0f},
        {.action = Action::Pick,
         .family = ActionFamily::Pick,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::Gag, .family = ActionFamily::Gag, .traits = kAutoMelee},
        {.action = Action::ShieldReady,
         .family = ActionFamily::Shield,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::ShieldRun,
         .family = ActionFamily::Shield,
         .pace = kRunPace,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::DeathGrabStart,
         .family = ActionFamily::DeathGrab,
         .pace = 0.0f,
         .traits = kBlendsToReady},
        {.action = Action::DeathGrab,
         .family = ActionFamily::DeathGrab,
         .pace = 0.0f,
         .traits = kBlendsToReady},
        {.action = Action::DeathGrabRelease,
         .family = ActionFamily::DeathGrab,
         .pace = 0.0f,
         .traits = kBlendsToReady},
        // Flung up, it gets up as from a fall onto its back (action.c 1270).
        {.action = Action::Whirled,
         .family = ActionFamily::Floored,
         .pace = 0.0f,
         .next = Action::GetUpBack,
         .traits = kKeepsPress},
        {.action = Action::Pushed,
         .family = ActionFamily::Pushed,
         .traits = kBlendsToReady | kAutoMelee},
        {.action = Action::ComboAct1,
         .family = ActionFamily::ComboAct,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboAct2,
         .family = ActionFamily::ComboAct,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboAct3,
         .family = ActionFamily::ComboAct,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboWar1,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboWar2,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboWar3,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboVal,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboWiz,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboArc,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboDwf1,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboDwf2,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = kChargerTurn},
        {.action = Action::ComboDwf3,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboKni,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboSor,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
        {.action = Action::ComboJes,
         .family = ActionFamily::ComboBound,
         .attackType = AttackType::Combo,
         .pace = 0.0f,
         .turn = 0.0f},
    }};
    static_assert(
        [] {
            for (usize i = 0; i < kActions.size(); ++i) {
                if (kActions[i].action != static_cast<Action>(i)) {
                    return false;
                }
            }
            return true;
        }(),
        "each action's row sits at its own index");
    static_assert(kActions.back().action == Action::ComboJes, "the last row is the last action");
    static_assert(
        [] {
            for (const ActionInfo& row : kActions) {
                if (row.foot == Foot::None) {
                    if (row.otherHalf != Action::Ready) {
                        return false;
                    }
                    continue;
                }
                const ActionInfo& other = kActions[index(row.otherHalf)];
                if (other.otherHalf != row.action || other.foot == Foot::None ||
                    other.foot == row.foot) {
                    return false;
                }
            }
            return true;
        }(),
        "each half cycle's other half pairs back to it on the other foot");
    static_assert(
        std::ranges::none_of(kSequenceNames, [](std::string_view name) { return name.empty(); }),
        "every action names its sequence");
    // The row outside the actions: of no family, category or trait.
    static constexpr ActionInfo kNoAction{.action = Action::Ready, .family = ActionFamily::None};
    return index(action) < kActions.size() ? kActions[index(action)] : kNoAction;
}

bool PlayerAnimator::Checkpoint::valid() const {
    return animation.valid() && index(action) < kActionCount && footfall <= Foot::Second &&
           meleeBlow <= MeleeBlow::Power && melee.range <= MeleeRange::Swing &&
           std::isfinite(melee.yaw) && std::abs(melee.yaw) <= std::numbers::pi_v<f32> &&
           character >= -1 && character < kClassCount && chain >= 0 && chain <= kMaxTicks &&
           potionThrowTicks >= 0 && potionThrowTicks <= kMaxTicks && comboClass >= -1 &&
           comboClass < kClassCount && stillTicks >= 0 && stillTicks <= kMaxTicks &&
           fidgetTicks >= 0 && fidgetTicks <= kMaxTicks &&
           static_cast<usize>(itemReleased) < kPlayerDeedCount && strafe <= StrafeWay::Right &&
           std::isfinite(guardArmor) && guardArmor >= 0 && guardArmor <= kMaxGuardArmor &&
           std::isfinite(attackSeconds) && attackSeconds >= 0 &&
           attackSeconds <= AnimationPlayer::kCheckpointSeconds && std::isfinite(phaseSeconds) &&
           phaseSeconds >= 0 && phaseSeconds <= AnimationPlayer::kCheckpointSeconds &&
           std::isfinite(shotFrame) && shotFrame >= 0 &&
           shotFrame <= AnimationPlayer::kCheckpointFrames;
}

PlayerAnimator::Checkpoint PlayerAnimator::checkpoint() const {
    Checkpoint state;
    state.animation = m_player.checkpoint();
    state.action = m_current;
    state.footfall = m_footfall;
    state.meleeBlow = m_meleeBlow;
    state.melee = m_melee;
    state.character = m_character;
    state.chain = m_chain;
    state.potionThrowTicks = m_potionThrowTicks;
    state.comboClass = m_comboClass;
    state.stillTicks = m_stillTicks;
    state.fidgetTicks = m_fidgetTicks;
    state.itemReleased = m_itemReleased;
    state.strafe = m_strafe;
    state.guardArmor = m_guardArmor;
    state.attackSeconds = m_attackSeconds;
    state.phaseSeconds = m_phaseSeconds;
    state.shotFrame = m_shotFrame;
    state.entered = m_entered;
    state.released = m_released;
    state.quickPress = m_quickPress;
    state.strongPress = m_strongPress;
    state.quickHeld = m_quickHeld;
    state.strongHeld = m_strongHeld;
    state.moved = m_moved;
    state.shielded = m_shielded;
    state.pushed = m_pushed;
    state.potionUsed = m_potionUsed;
    state.potionThrown = m_potionThrown;
    state.potionThrowReleased = m_potionThrowReleased;
    state.dead = m_dead;
    state.turboBegan = m_turboBegan;
    state.strongReleased = m_strongReleased;
    state.potionShielded = m_potionShielded;
    state.shieldAsked = m_shieldAsked;
    state.legendAsked = m_legendAsked;
    state.legendReleased = m_legendReleased;
    state.superReleased = m_superReleased;
    state.superHeld = m_superHeld;
    state.rapid = m_rapid;
    state.speed = m_speed;
    state.comboRide = m_comboRide;
    state.potionLatch = m_potionLatch;
    return state;
}

bool PlayerAnimator::restore(const Checkpoint& state) {
    if (m_tree == nullptr || !state.valid() || !state.animation.playing ||
        state.animation.sequence >= m_tree->sequences.size() ||
        state.animation.sequence != sequenceOf(state.action)) {
        return false;
    }
    AnimationPlayer clock;
    if (!clock.restore(state.animation, &m_tree->sequences[state.animation.sequence])) {
        return false;
    }
    m_player = clock;
    m_current = state.action;
    m_footfall = state.footfall;
    m_meleeBlow = state.meleeBlow;
    m_melee = state.melee;
    m_character = state.character;
    m_chain = state.chain;
    m_potionThrowTicks = state.potionThrowTicks;
    m_comboClass = state.comboClass;
    m_stillTicks = state.stillTicks;
    m_fidgetTicks = state.fidgetTicks;
    m_itemReleased = state.itemReleased;
    m_strafe = state.strafe;
    m_guardArmor = state.guardArmor;
    m_attackSeconds = state.attackSeconds;
    m_phaseSeconds = state.phaseSeconds;
    m_shotFrame = state.shotFrame;
    m_entered = state.entered;
    m_released = state.released;
    m_quickPress = state.quickPress;
    m_strongPress = state.strongPress;
    m_quickHeld = state.quickHeld;
    m_strongHeld = state.strongHeld;
    m_moved = state.moved;
    m_shielded = state.shielded;
    m_pushed = state.pushed;
    m_potionUsed = state.potionUsed;
    m_potionThrown = state.potionThrown;
    m_potionThrowReleased = state.potionThrowReleased;
    m_dead = state.dead;
    m_turboBegan = state.turboBegan;
    m_strongReleased = state.strongReleased;
    m_potionShielded = state.potionShielded;
    m_shieldAsked = state.shieldAsked;
    m_legendAsked = state.legendAsked;
    m_legendReleased = state.legendReleased;
    m_superReleased = state.superReleased;
    m_superHeld = state.superHeld;
    m_rapid = state.rapid;
    m_speed = state.speed;
    m_comboRide = state.comboRide;
    m_potionLatch = state.potionLatch;
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    m_previous = m_pose;
    m_presentationPrevious = m_pose;
    m_presentationGeneration = m_player.generation();
    return true;
}

bool PlayerAnimator::TransitionPose::valid() const {
    return current.valid() && previous.valid() && current.tree == previous.tree &&
           current.nodes == previous.nodes;
}

std::optional<PlayerAnimator::TransitionPose> PlayerAnimator::transitionPose() const {
    if (m_tree == nullptr || !m_player.transitioning()) {
        return std::nullopt;
    }
    auto current = m_pose.checkpoint();
    auto previous = m_previous.checkpoint();
    if (!current || !previous) {
        return std::nullopt;
    }
    TransitionPose state{std::move(*current), std::move(*previous)};
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

bool PlayerAnimator::isBlockableAttack(Action action) {
    // PlayerAttacking(level 1): categories 2, 5, 10, and 11 or higher.
    // Recoveries retain their attack category; quick, step and low swings do not qualify.
    const AttackType type = info(action).attackType;
    return type == AttackType::Slow || type == AttackType::Spin ||
           type == AttackType::StrongThrow || type == AttackType::Special ||
           type == AttackType::Combo;
}

bool PlayerAnimator::locomotion(Action action) {
    const ActionFamily family = info(action).family;
    return family == ActionFamily::Stance || family == ActionFamily::Fidget ||
           family == ActionFamily::Walk || family == ActionFamily::Run ||
           family == ActionFamily::StrafeWalk || family == ActionFamily::Shield;
}

bool PlayerAnimator::rollbackTurboAction(Action action) {
    return info(action).family == ActionFamily::Turbo;
}

bool PlayerAnimator::rollbackShoveAction(Action action) {
    return action == Action::Shove;
}

bool PlayerAnimator::isThrow(Action action) {
    return info(action).family == ActionFamily::Throw;
}

bool PlayerAnimator::isMelee(Action action) {
    return info(action).family == ActionFamily::Melee;
}

bool PlayerAnimator::comboing() const {
    return info(m_current).family == ActionFamily::ComboAct;
}

bool PlayerAnimator::comboBound() const {
    return info(m_current).family == ActionFamily::ComboBound;
}

bool PlayerAnimator::grabbingDeath() const {
    return info(m_current).family == ActionFamily::DeathGrab;
}

bool PlayerAnimator::strafing() const {
    const ActionFamily family = info(m_current).family;
    return family == ActionFamily::StrafeWalk || family == ActionFamily::StrafeShoot;
}

bool PlayerAnimator::floored() const {
    return info(m_current).family == ActionFamily::Floored;
}

bool PlayerAnimator::conjuring() const {
    return info(m_current).family == ActionFamily::Potion;
}

bool PlayerAnimator::lunging() const {
    return info(m_current).attackType == AttackType::Step;
}

bool PlayerAnimator::itemAttacking() const {
    return info(m_current).family == ActionFamily::Item;
}

f32 PlayerAnimator::moveScale() const {
    if (entering()) {
        return 0.0f;
    }
    const ActionInfo& row = info(m_current);
    const PowerMoves& power = powerMovesOf(m_character);
    switch (row.motion) {
    case MotionRule::Fixed:
    case MotionRule::SpellStorm: return row.pace;
    case MotionRule::Step: return m_moved ? row.pace : kIdleStepPace;
    case MotionRule::PowerClose: return power.rootedClose ? 0.0f : power.rate;
    case MotionRule::PowerMed: return power.rootedMed ? 0.0f : power.medRate;
    }
    return row.pace;
}

f32 PlayerAnimator::turnScale() const {
    const ActionInfo& row = info(m_current);
    const PowerMoves& power = powerMovesOf(m_character);
    switch (row.motion) {
    case MotionRule::Fixed:
    case MotionRule::Step: return row.turn;
    case MotionRule::PowerClose: return power.rootedClose ? 0.0f : row.turn;
    case MotionRule::PowerMed: return power.rootedMed ? 0.0f : row.turn;
    case MotionRule::SpellStorm:
        // DoPlayerAction's A4C: full moves turn slowly; Spell Storm fixes its
        // heading once the first knight's birth begins, strictly after frame 11.
        return m_character == kSorceressClass && m_player.frame() > kSpellStormLockFrame ? 0.0f
                                                                                         : row.turn;
    }
    return row.turn;
}

bool PlayerAnimator::bind(const TreeInfo& tree, bool enter) {
    unbind();
    for (usize a = 0; a < kActionCount; ++a) {
        const auto sequence = tree.findSequence(kSequenceNames[a]);
        m_sequences[a] = sequence.has_value() ? static_cast<s32>(*sequence) : -1;
    }
    if (m_sequences[index(Action::Ready)] < 0) {
        return false;
    }
    m_tree = &tree;
    m_entered = !enter;
    const u32 stance = sequenceOf(Action::Ready);
    // DoPlayerAction enables AnimationInfo.flags bit 2 for every player action.
    m_player.setSmooth(true);
    m_player.start(tree.sequences[stance], stance);
    m_pose.evaluate(tree, stance, 0.0f);
    m_previous = m_pose;
    m_presentationPrevious = m_pose;
    m_presentationGeneration = m_player.generation();
    return true;
}

void PlayerAnimator::unbind() {
    m_tree = nullptr;
    m_sequences.fill(-1);
    m_current = Action::Ready;
    m_entered = true;
    m_stillTicks = 0;
    m_fidgetTicks = 0;
    m_released = false;
    m_meleeBlow = MeleeBlow::None;
    m_chain = 0;
    m_quickPress = false;
    m_strongPress = false;
    m_quickHeld = false;
    m_strongHeld = false;
    m_moved = false;
    m_potionUsed = false;
    m_potionThrown = false;
    m_potionLatch = false;
    m_potionThrowTicks = 0;
    m_potionThrowReleased = false;
    m_dead = false;
    m_turboBegan = false;
    m_strongReleased = false;
    m_potionShielded = false;
    m_shieldAsked = false;
    m_legendAsked = false;
    m_legendReleased = false;
    m_superReleased = false;
    m_superHeld = false;
    m_itemReleased = PlayerDeed::None;
    m_rapid = false;
    m_speed = false;
    m_guardArmor = 5.0f;
    m_strafe = StrafeWay::None;
    m_comboClass = -1;
    m_comboRide = false;
    m_attackSeconds = 0.0f;
    m_phaseSeconds = 0.0;
    m_shotFrame = 0.0f;
    m_player.setSpeed(1.0f);
    m_player.stop();
    m_presentationPrevious = TreePose{};
}

void PlayerAnimator::evaluatePresentation(TreePose& out, f32 alpha) const {
    if (!bound()) {
        out = TreePose{};
        return;
    }
    out.evaluate(*m_tree, m_player.sequence(), m_player.presentationFrame(), false, true);
    if (m_player.transitioning()) {
        out.blend(m_previous, m_player.transition());
    }
    if (alpha < 1.0f && m_presentationPrevious.posed() &&
        m_presentationGeneration == m_player.generation()) {
        out.blend(m_presentationPrevious, std::clamp(alpha, 0.0f, 1.0f), true);
    }
}

void PlayerAnimator::advancePresentation(f32 seconds) {
    if (!bound() || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    evaluatePresentation(m_presentationPrevious);
    m_presentationGeneration = m_player.generation();
    // Keep the current action's loop policy without applying its continuation
    // or polling inputs. Native Legends still calls AnimateTree while life5
    // holds the actor outside its gameplay action dispatcher (e0042614).
    m_player.advance(seconds, decide(m_current).repeat);
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    if (m_player.transitioning()) {
        m_pose.blend(m_previous, m_player.transition());
    }
    m_meleeBlow = MeleeBlow::None;
    m_footfall = Foot::None;
    m_released = false;
    m_strongReleased = false;
    m_superReleased = false;
    m_legendReleased = false;
    m_itemReleased = PlayerDeed::None;
    m_potionUsed = false;
    m_potionThrown = false;
    m_potionShielded = false;
    m_turboBegan = false;
}

PlayerAnimator::Action PlayerAnimator::comboHeldActionOf(s32 grabberClass) {
    switch (grabberClass) {
    case kWarriorClass: return Action::ComboWar1;
    case kValkyrieClass: return Action::ComboVal;
    case kWizardClass: return Action::ComboWiz;
    case kArcherClass: return Action::ComboArc;
    case kDwarfClass: return Action::ComboDwf1;
    case kKnightClass: return Action::ComboKni;
    case kSorceressClass: return Action::ComboSor;
    case kJesterClass: return Action::ComboJes;
    default: return Action::Ready;
    }
}

PlayerAnimator::Action PlayerAnimator::comboThrownActionOf(s32 grabberClass) {
    switch (grabberClass) {
    case kWarriorClass: return Action::ComboWar2;
    case kDwarfClass: return Action::ComboDwf2;
    default: return Action::Ready;
    }
}

PlayerAnimator::Action PlayerAnimator::strafeStep(StrafeWay way, bool shooting) {
    // The first half of each way from Forward, in StrafeWay's order: walking, then shooting.
    static constexpr std::array<std::array<Action, 4>, 2> kFirstHalves{{
        {Action::StrafeForward1, Action::StrafeBack1, Action::StrafeLeft1, Action::StrafeRight1},
        {Action::StrafeShootForward1, Action::StrafeShootBack1, Action::StrafeShootLeft1,
         Action::StrafeShootRight1},
    }};
    const auto& halves = kFirstHalves[shooting ? 1 : 0];
    // No way at all steps forward.
    const usize column = way == StrafeWay::None ? 0 : static_cast<usize>(way) - 1;
    return column < halves.size() ? halves[column] : halves[0];
}

PlayerAnimator::Action PlayerAnimator::firstHalfOf(Action step) {
    const ActionInfo& row = info(step);
    return row.foot == Foot::Second ? row.otherHalf : step;
}

PlayerAnimator::Action PlayerAnimator::otherHalfOf(Action step) {
    return info(step).otherHalf;
}

PlayerAnimator::Action PlayerAnimator::turboActionOf(PlayerDeed deed) {
    switch (deed) {
    case PlayerDeed::TurboStrong: return Action::TurboStrong;
    case PlayerDeed::TurboFull: return Action::TurboFull;
    case PlayerDeed::Shove: return Action::Shove;
    case PlayerDeed::StrongAttack: return Action::StrongThrow;
    case PlayerDeed::HurlLegend: return Action::UsePotion;
    case PlayerDeed::ThrowLegend: return Action::StrongThrow;
    case PlayerDeed::ShootLegend:
    case PlayerDeed::SuperShot: return Action::SpecialShot;
    case PlayerDeed::Hammer: return Action::Hammer;
    case PlayerDeed::Breathe: return Action::Breathe;
    case PlayerDeed::FireLeft: return Action::FireLeft;
    case PlayerDeed::FireRight: return Action::FireRight;
    case PlayerDeed::Combo: return Action::ComboAct1;
    default: return Action::Ready;
    }
}

bool PlayerAnimator::ordinaryAttack() const {
    return !m_legendAsked && ordinaryAttackType(info(m_current).attackType);
}

bool PlayerAnimator::canBegin(PlayerDeed deed) const {
    const Action action = turboActionOf(deed);
    if (action == Action::Ready || !bound() || !playable(action) || entering() || conjuring() ||
        reacting() || comboBound() || dying()) {
        return false;
    }
    // DoPlayerAction (800AC068) resets current attack categories 1..10 to
    // READY with mode 2 for a category >=11 request. Do not lose a one-tick
    // turbo press while an ordinary throw, heavy throw or gauntlet is playing.
    if (deedHas(deed, deed_trait::kCutsAttack) && ordinaryAttack()) {
        return true;
    }
    // The remaining requests retain their narrower interruption rules.
    const bool cutsMelee = deedHas(deed, deed_trait::kCutsMelee);
    const bool cutsGuard = deedHas(deed, deed_trait::kCutsGuard);
    return !throwing() && (!meleeing() || cutsMelee) && !turboing() && (!guarding() || cutsGuard);
}

PlayerMotion PlayerAnimator::motionFor(f32 stickMagnitude) {
    if (stickMagnitude > kRunMagnitude) {
        return PlayerMotion::Run;
    }
    return stickMagnitude > 0.0f ? PlayerMotion::Walk : PlayerMotion::Stand;
}

u32 PlayerAnimator::sequenceOf(Action action) const {
    s32 sequence = m_sequences[index(action)];
    // A class without the low power swing makes the close one.
    if (sequence < 0 && action == Action::PowerLow) {
        sequence = m_sequences[index(Action::PowerClose)];
    } else if (sequence < 0 && action == Action::PowerLowRecover) {
        sequence = m_sequences[index(Action::PowerCloseRecover)];
    }
    return sequence >= 0 ? static_cast<u32>(sequence)
                         : static_cast<u32>(m_sequences[index(Action::Ready)]);
}

void PlayerAnimator::update(PlayerMotion motion, s32 ticks, f32 seconds, PlayerDeed deed,
                            bool chainHeldMelee) {
    m_meleeBlow = MeleeBlow::None;
    if (!bound()) {
        return;
    }
    evaluatePresentation(m_presentationPrevious);
    m_presentationGeneration = m_player.generation();
    if (deedHas(deed, deed_trait::kAutomatic) &&
        (motion == PlayerMotion::Stand || m_strafe != StrafeWay::None || !canAutoMelee())) {
        deed = PlayerDeed::None;
    }
    // A press is a button going down; it stays fresh until the next strike begins.
    const bool quickHeld = deedHas(deed, deed_trait::kAttackButton);
    const bool strongHeld = deedHas(deed, deed_trait::kStrongButton);
    // The optional automatic-melee mode also carries held close swings through a
    // combo. Manual controls retain the native fresh-press latch, and ranged
    // attacks never advance a melee combo merely because their button is held.
    const bool continuingMelee = chainHeldMelee && deedHas(deed, deed_trait::kClose) &&
                                 deedHas(deed, deed_trait::kAttackButton);
    m_quickPress = m_quickPress || (quickHeld && !m_quickHeld) || continuingMelee;
    m_strongPress = m_strongPress || (strongHeld && !m_strongHeld);
    m_quickHeld = quickHeld;
    m_strongHeld = strongHeld;
    m_moved = motion != PlayerMotion::Stand;
    if (!meleeing()) {
        m_chain = 0;
    }
    m_footfall = Foot::None;
    m_released = false;
    m_potionUsed = false;
    m_potionThrown = false;
    m_turboBegan = false;
    // THROWPOTIONS accumulates throw_str while held; letting go latches its value
    // until the release sequence creates the bottle, even if the button is pressed again.
    if (m_current == Action::ThrowPotion) {
        if (deed != PlayerDeed::ThrowPotion && deed != PlayerDeed::UsePotion) {
            m_potionThrowReleased = true;
        } else if (!m_potionThrowReleased) {
            m_potionThrowTicks += std::max(0, ticks);
        }
    }
    m_strongReleased = false;
    m_potionShielded = false;
    m_legendReleased = false;
    m_superReleased = false;
    m_itemReleased = PlayerDeed::None;
    m_superHeld = deed == PlayerDeed::SuperShot;
    if (deed == PlayerDeed::Die || dying()) {
        // Nothing else is asked of a body that falls: it plays through once and stays down.
        if (m_sequences[index(Action::Death)] < 0) {
            m_dead = true;
            return;
        }
        Decision fall{Action::Death};
        fall.cut = dying() ? Cut::WhenDoneIfDifferent : Cut::Now;
        if (!m_dead) {
            play(fall, seconds);
            m_released = false; // what the hand held falls with it
            m_potionUsed = false;
            m_potionThrown = false;
            m_dead = dying() && m_player.finished();
            m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        }
        return;
    }
    if (deed == PlayerDeed::Grabbed || deed == PlayerDeed::Thrown) {
        Decision captured{deed == PlayerDeed::Grabbed ? Action::Grabbed : Action::FallBack};
        captured.cut = m_current == captured.action ? Cut::WhenDoneIfDifferent : Cut::Now;
        captured.repeat = deed == PlayerDeed::Grabbed;
        play(captured, seconds);
        m_released = false;
        m_strongReleased = false;
        m_potionUsed = false;
        m_potionThrown = false;
        m_potionShielded = false;
        m_legendAsked = false;
        m_legendReleased = false;
        m_shieldAsked = false;
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        return;
    }
    if (m_current == Action::Grabbed) {
        Decision freed;
        freed.cut = Cut::Now;
        play(freed, 0);
    }
    // A partner's combo owns the body: held, it plays the grabber's class's sequence once
    // and holds; let go of, the warrior's pinball or the dwarf's charger loops until the
    // flight or the ride is over, then lands (fn_80088938's forced actions 38 and 39).
    if (deed == PlayerDeed::ComboHeld || deed == PlayerDeed::ComboThrown) {
        const Action asked = deed == PlayerDeed::ComboHeld ? comboHeldActionOf(m_comboClass)
                                                           : comboThrownActionOf(m_comboClass);
        if (asked != Action::Ready && playable(asked)) {
            play(decide(asked), seconds);
        }
        m_released = false;
        m_strongReleased = false;
        m_potionUsed = false;
        m_potionThrown = false;
        m_potionShielded = false;
        m_legendAsked = false;
        m_shieldAsked = false;
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        return;
    }
    // A hit cuts into anything at once, unless one is already being reeled from; while it
    // plays nothing else is asked of the body.
    const bool felled = deedHas(deed, deed_trait::kFells);
    const bool struck = deedHas(deed, deed_trait::kStrikes);
    if (struck && !floored() &&
        (felled || !reacting() || (deed == PlayerDeed::Webbed && webbed()))) {
        Action reaction = deed == PlayerDeed::Flinch ? Action::HitReact : Action::Stun;
        if (deed == PlayerDeed::Spike) {
            reaction = Action::SpikeHit;
        }
        if (deed == PlayerDeed::Webbed) {
            reaction = Action::WebReact;
        }
        if (felled) {
            reaction = deed == PlayerDeed::FallBack ? Action::FallBack : Action::FallForward;
        }
        if (deed == PlayerDeed::Whirled) {
            reaction = Action::Whirled;
        }
        if (m_sequences[index(reaction)] >= 0) {
            Decision reel{reaction};
            reel.cut = webbed() ? Cut::IfDifferent : Cut::Now;
            reel.repeat = reaction == Action::WebReact;
            play(reel, seconds);
            m_released = false; // a throw it cut into never leaves the hand
            m_potionUsed = false;
            m_potionThrown = false;
            m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
            return;
        }
    }
    // A turbo move cuts into standing, walking and running at once and plays through; a
    // legend item's gesture is made the same way, but pays and lets go of nothing.
    if (canBegin(deed)) {
        Decision move{turboActionOf(deed)};
        move.cut = Cut::Now;
        m_legendAsked = deedHas(deed, deed_trait::kLegend);
        m_shieldAsked = false;
        play(move, seconds);
        m_turboBegan = !m_legendAsked && deed != PlayerDeed::SuperShot && !itemAttacking();
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        return;
    }
    // S_DEFEND is a press, not a held block. The gesture starts here; decide()
    // completes its three phases even if the button is released immediately.
    // DoPlayerAction dispatches a new guard through Ready during attack
    // categories 1..10, not just recoveries. Strong throws, gauntlets and
    // shoves qualify even though turboing() also locks their animations;
    // true turbos, special shots, hammer, breath and combos do not.
    const bool interruptibleAttack = throwing() || meleeing() || m_current == Action::Shove ||
                                     info(m_current).attackType == AttackType::StrongThrow;
    const bool free =
        !entering() && !conjuring() && !reacting() && (!turboing() || interruptibleAttack);
    const bool asked =
        deed == PlayerDeed::Defend && free && m_sequences[index(Action::Defend)] >= 0;
    if (asked) {
        Decision guard{Action::DefendRaise};
        guard.action = playable(Action::DefendRaise) ? Action::DefendRaise : Action::Defend;
        guard.cut = Cut::Now;
        play(guard, seconds);
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        if (m_player.transitioning()) {
            m_pose.blend(m_previous, m_player.transition());
        }
        return;
    }
    const bool turboAsked = deedHas(deed, deed_trait::kHoldsBody);
    const bool potionInterrupt = ordinaryAttack() && deedHas(deed, deed_trait::kPotion);
    // Locking the body below must not erase the request used by DoPlayerAction's
    // speed selection. Native fn_80088938 retains held attack bits even during
    // strong/item attacks, and PlayerMotion selects their requested action again.
    Action lockedSpeedRequest = Action::Ready;
    if (turboing() || guarding()) {
        lockedSpeedRequest = turboActionOf(deed);
        if (deed == PlayerDeed::Attack && playable(Action::Throw)) {
            lockedSpeedRequest = m_strafe != StrafeWay::None && motion != PlayerMotion::Stand
                                     ? strafeStep(m_strafe, true)
                                     : Action::Throw;
        } else if (deed == PlayerDeed::Defend) {
            lockedSpeedRequest = Action::Defend;
        }
    }
    if (reacting() || struck || (turboing() && !potionInterrupt) || turboAsked) {
        deed = PlayerDeed::None;
        motion = PlayerMotion::Stand;
    }
    bool attack = deed == PlayerDeed::Attack;
    Action melee = meleeRequest(deed, m_moved);
    if (!playable(melee)) {
        melee = Action::Ready;
    }
    // A class without a deed's sequences does not do it.
    // One potion a press: the button must come up before it asks for another.
    const bool forShield = deed == PlayerDeed::ShieldPotion;
    if (deed != PlayerDeed::UsePotion && deed != PlayerDeed::ThrowPotion && !forShield) {
        m_potionLatch = false;
    }
    // A shield is raised with the same gesture as a potion is used.
    const bool use = (deed == PlayerDeed::UsePotion || forShield) && !m_potionLatch &&
                     m_sequences[index(Action::UsePotion)] >= 0;
    if (use) {
        m_shieldAsked = forShield;
    }
    const bool toss = deed == PlayerDeed::ThrowPotion && !m_potionLatch &&
                      m_sequences[index(Action::ThrowPotion)] >= 0;
    if (throwing()) {
        m_attackSeconds += seconds;
    }
    // A class without the throw's sequences does not throw.
    attack = attack && m_sequences[index(Action::Throw)] >= 0;
    const bool stepping =
        m_strafe != StrafeWay::None && motion != PlayerMotion::Stand && !meleeing() && !conjuring();
    if (stepping && attack && m_sequences[index(strafeStep(m_strafe, true))] >= 0) {
        play(decide(strafeStep(m_strafe, true)), seconds);
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        if (m_player.transitioning()) {
            m_pose.blend(m_previous, m_player.transition());
        }
        return;
    }
    if ((attack && !stepping) || throwing() || meleeing() || melee != Action::Ready || use ||
        toss || conjuring()) {
        motion = PlayerMotion::Stand;
        m_stillTicks = 0;
        m_fidgetTicks = 0;
    }
    // Standing still counts up to the first fidget; after it the second timer takes over.
    if (motion == PlayerMotion::Stand) {
        if (m_fidgetTicks == 0) {
            m_stillTicks += ticks;
        } else {
            m_fidgetTicks += ticks;
        }
    } else {
        m_stillTicks = 0;
        m_fidgetTicks = 0;
    }
    Action requested = Action::Ready;
    if (!m_entered) {
        requested = Action::Start;
    } else if (use) {
        requested = Action::UsePotion;
    } else if (toss) {
        requested = Action::ThrowPotion;
    } else if (melee != Action::Ready) {
        requested = melee;
    } else if (deed == PlayerDeed::DeathGrab && playable(Action::DeathGrabStart)) {
        requested = Action::DeathGrabStart;
    } else if ((deed == PlayerDeed::Pick && playable(Action::Pick)) ||
               (deed == PlayerDeed::Gag && playable(Action::Gag))) {
        requested = deed == PlayerDeed::Pick ? Action::Pick : Action::Gag;
    } else if (m_strafe != StrafeWay::None && motion != PlayerMotion::Stand &&
               m_sequences[index(strafeStep(m_strafe, false))] >= 0) {
        requested = strafeStep(m_strafe, false);
    } else if (attack) {
        requested = Action::Throw;
    } else if (motion == PlayerMotion::Run) {
        requested = Action::Run1;
    } else if (motion == PlayerMotion::Walk) {
        requested = Action::Walk1;
    }
    // A shield on the arm has a stance and a gait of its own (AnimAction, action.c 1296).
    if (m_shielded && requested == Action::Ready && playable(Action::ShieldReady)) {
        requested = Action::ShieldReady;
    } else if (m_shielded && (requested == Action::Walk1 || requested == Action::Run1) &&
               playable(Action::ShieldRun)) {
        requested = Action::ShieldRun;
    }
    const bool afoot = requested == Action::Ready || requested == Action::ShieldReady ||
                       requested == Action::Walk1 || requested == Action::Run1 ||
                       requested == Action::ShieldRun;
    if (m_pushed && afoot && playable(Action::Pushed)) {
        requested = Action::Pushed;
    }
    auto decision = decide(requested);
    if (lockedSpeedRequest != Action::Ready) {
        decision.requested = lockedSpeedRequest;
    }
    play(decision, seconds);
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    if (m_player.transitioning()) {
        m_pose.blend(m_previous, m_player.transition());
    }
}

PlayerAnimator::Decision PlayerAnimator::decide(Action requested) const {
    // Native requests at/after P_USE_MAGIC replace an ordinary attack's
    // dispatch state with READY before its phase can choose a continuation.
    if (ordinaryAttack() && (requested == Action::UsePotion || requested == Action::ThrowPotion)) {
        Decision d{requested};
        d.cut = Cut::IfDifferent;
        return d;
    }
    Decision d = continuation(requested);
    applyCutIns(requested, d);
    if (d.action == Action::Ready && m_current != Action::Ready) {
        d.transition = readyTransition();
    }
    if (d.action != m_current) {
        d.action = refine(d.action);
    }
    return d;
}

PlayerAnimator::Decision PlayerAnimator::continuation(Action requested) const {
    Decision d{requested};
    switch (m_current) {
    case Action::Ready:
        d.repeat = true;
        d.cut = Cut::IfDifferent;
        if (requested == Action::Ready) {
            if (m_stillTicks > kFidgetTicks) {
                d.action = Action::Idle1;
                d.cut = Cut::WhenDone;
            } else if (m_fidgetTicks > kSecondFidgetTicks) {
                d.action = Action::Idle2;
                d.cut = Cut::WhenDone;
            }
        }
        break;
    case Action::Idle1:
        d.cut = Cut::IfDifferent;
        if (requested == Action::Ready) {
            d.action = Action::Ready;
            d.cut = Cut::WhenDone;
        }
        break;
    case Action::Idle2:
        d.cut = Cut::IfDifferent;
        if (requested == Action::Ready) {
            d.action = Action::Idle2Loop;
            d.cut = Cut::WhenDone;
        }
        break;
    case Action::Idle2Loop:
        d.repeat = true;
        d.cut = Cut::IfDifferent;
        if (requested == Action::Ready) {
            d.action = Action::Idle2Loop;
            d.cut = Cut::WhenDoneIfDifferent;
        }
        break;
    case Action::Quick1:
    case Action::Quick2:
    case Action::Quick3:
    case Action::Step1:
    case Action::Step2:
    case Action::Step3:
    case Action::WalkStrike: d.action = chainAfter(m_current); break;
    case Action::Quick2Recover:
    case Action::Quick3Recover:
    case Action::Step2Recover:
    case Action::Step3Recover:
    case Action::WalkStrikeRecover:
        // A strong press buffered in the chain makes a power swing once recovered; a fresh
        // press early in the recovery swings again at once.
        if (buffered()) {
            d.action = powerOf(m_chain);
        } else if ((m_quickPress || m_strongPress) && m_player.frame() <= kReleaseFrame &&
                   m_melee.range == MeleeRange::Swing) {
            const bool second =
                m_current == Action::Quick2Recover || m_current == Action::Step2Recover;
            d.action = second ? Action::Quick3 : Action::Quick2;
            d.cut = Cut::IfDifferent;
        }
        break;
    case Action::Spin: d.action = buffered() ? Action::PowerMed : Action::SpinRecover; break;
    case Action::LowKick: d.action = buffered() ? Action::PowerLow : Action::LowKickRecover; break;
    case Action::LowKickRecover:
        // P_ATTACK_KICK_R retains the kick's buffered low-finisher branch.
        if (buffered()) {
            d.action = Action::PowerLow;
        } else {
            d.cut = Cut::WhenDone;
        }
        break;
    case Action::Low1:
    case Action::Low2:
        d.action = Action::LowRecover;
        if (requested == Action::Low1) {
            d.action = m_current == Action::Low1 ? Action::Low2 : Action::Low1;
        }
        break;
    case Action::Throw:
    case Action::ThrowMoving:
        // A close attack or moving shot takes over the wind-up at its current frame
        // (DoPlayerAction, P_THROW/P_THROWQ's non-throw attack branch).
        if (isMelee(requested) || info(requested).family == ActionFamily::StrafeShoot) {
            d.cut = Cut::IfDifferent;
            d.startFrame = std::round(m_player.frame());
            break;
        }
        // The wind-up gives way to the release at its end, or at once from its second frame.
        d.action = m_current == Action::Throw ? Action::ThrowRelease : Action::ThrowMovingRelease;
        d.cut = m_shotFrame >= kReleaseFrame ? Cut::IfDifferent : Cut::WhenDoneIfDifferent;
        d.shotBoundary = true;
        break;
    case Action::Defend:
        // DoPlayerAction 800AC2A8: a press plays DEFEND1 -> DEFEND2 -> DEFENDR, both
        // transitions in mode 1 whatever is asked next (the raising's by its row).
        d.action = playable(Action::DefendLower) ? Action::DefendLower : Action::Ready;
        d.cut = Cut::WhenDone;
        break;
    case Action::SpecialShot:
    case Action::SpecialShotRepeat:
        d.action =
            !m_legendAsked && m_superHeld && m_sequences[index(Action::SpecialShotRepeat)] >= 0
                ? Action::SpecialShotRepeat
                : Action::SpecialShotRecover;
        d.cut = Cut::WhenDone;
        break;
    case Action::ShieldReady:
        d.repeat = requested == Action::ShieldReady;
        d.cut = Cut::IfDifferent;
        break;
    case Action::ShieldRun: d.repeat = requested == Action::ShieldRun; break;
    // Pushed loops while it is asked for (action.c 1197).
    case Action::Pushed:
        d.repeat = requested == Action::Pushed;
        d.cut = Cut::IfDifferent;
        break;
    // Reached out, the hold loops while Death is held, then lets go at once (action.c 1220).
    case Action::DeathGrabStart:
        d.action =
            requested == Action::DeathGrabStart ? Action::DeathGrab : Action::DeathGrabRelease;
        break;
    case Action::DeathGrab:
        if (requested == Action::DeathGrabStart) {
            d.action = Action::DeathGrab;
            d.repeat = true;
        } else {
            d.action = Action::DeathGrabRelease;
            d.cut = Cut::IfDifferent;
        }
        break;
    case Action::Gag:
        // Looped while asked for; let go of, it ends its cycle, and past its first frames
        // anything else cuts in (P_DEATH_REACT).
        d.repeat = requested == Action::Gag;
        d.cut = requested == Action::Ready || m_player.frame() < kGagHold ? Cut::WhenDoneIfDifferent
                                                                          : Cut::IfDifferent;
        break;
    // The combo move (action.c 998): COMBOACT1 gives way when done to COMBOACT2, else to
    // COMBOACT3, else to whatever is asked; COMBOACT2 loops while the ride goes on and then
    // COMBOACT3 (or whatever is asked) cuts in at once; COMBOACT3 plays through.
    case Action::ComboAct1:
        if (playable(Action::ComboAct2)) {
            d.action = Action::ComboAct2;
            d.cut = Cut::WhenDone;
        } else if (playable(Action::ComboAct3)) {
            d.action = Action::ComboAct3;
            d.cut = Cut::WhenDone;
        }
        break;
    case Action::ComboAct2:
        if (m_comboRide) {
            d.action = Action::ComboAct2;
            d.repeat = true;
        } else {
            d.action = playable(Action::ComboAct3) ? Action::ComboAct3 : requested;
            d.cut = Cut::IfDifferent;
        }
        break;
    // The pinball and the charger loop while they are asked for, then land at once.
    case Action::ComboWar2:
    case Action::ComboDwf2:
        if (requested == m_current) {
            d.repeat = true;
        } else {
            const Action landing =
                m_current == Action::ComboWar2 ? Action::ComboWar3 : Action::ComboDwf3;
            d.action = playable(landing) ? landing : requested;
            d.cut = Cut::IfDifferent;
        }
        break;
    default: {
        // The rest give way by their rows: to the action of their own, else to whatever is
        // asked, by the row's cut; a half cycle gives way to its other half while the same
        // way is kept.
        const ActionInfo& row = info(m_current);
        if (row.next != Action::Ready) {
            d.action = row.next;
        }
        d.cut = row.cut;
        d.shotBoundary = (row.traits & action_trait::kShotBoundary) != 0;
        if (row.foot != Foot::None && requested == firstHalfOf(m_current)) {
            d.action = row.otherHalf;
        }
        // AnimAction's walking-strafe cases use mode 2 for an attack: it cuts in
        // immediately. Waiting for the footstep's end loses short button presses.
        // Shooting strafes still finish each half before releasing another weapon.
        if (row.family == ActionFamily::StrafeWalk &&
            info(requested).family == ActionFamily::StrafeShoot) {
            d.cut = Cut::IfDifferent;
        }
        break;
    }
    }
    return d;
}

void PlayerAnimator::applyCutIns(Action requested, Decision& d) const {
    // A partner's combo takes the body at once (action.c 558's mode 2 for P_FALL_DOWN to
    // P_GRABBED), once and not looped.
    if (info(requested).family == ActionFamily::ComboBound && d.action == requested &&
        !comboBound()) {
        d.cut = Cut::IfDifferent;
        d.repeat = false;
    }
    // A potion cuts into standing, walking and running at once, as an attack does.
    // Nothing cuts the pickup's gesture short (P_PICKUP waits for its end).
    const bool picking = m_current == Action::Pick;
    if ((requested == Action::UsePotion || requested == Action::ThrowPotion) &&
        d.action == requested && !isThrow(m_current) && !conjuring() &&
        m_current != Action::Start && !picking) {
        d.cut = Cut::IfDifferent;
    }
    // An attack cuts into walking and running at once, and from their first halves takes the
    // moving wind-up.
    if (requested == Action::Throw && d.action == Action::Throw && !isThrow(m_current)) {
        if (m_current != Action::Start && !meleeing() && !strafing() && !picking && !guarding()) {
            d.cut = Cut::IfDifferent;
        }
        if (m_current == Action::Walk1 || m_current == Action::Run1) {
            d.action = Action::ThrowMoving;
        }
    }
    if (isMelee(requested) && d.action == requested && !meleeing() && !throwing() && !conjuring() &&
        !reacting() && !turboing() && !entering() && !picking && !guarding()) {
        d.cut = Cut::IfDifferent;
    }
    // Death is taken hold of at once (P_DEATHGRAB's mode 2).
    if (requested == Action::DeathGrabStart && d.action == requested && !grabbingDeath() &&
        !entering() && !reacting()) {
        d.cut = Cut::IfDifferent;
    }
}

f32 PlayerAnimator::readyTransition() const {
    // Only ordinary locomotion and close attacks blend back to the stance.
    // Reactions, ranged attacks, magic, turbos and combos end directly.
    if (m_current == Action::Quick2Recover && m_character == kArcherClass) {
        return 0.0f;
    }
    return (info(m_current).traits & action_trait::kBlendsToReady) != 0 ? kStanceBlend : 0.0f;
}

/** The close attack a deed asks for (AnimAction's requests): a step into what is a pace
 * away while the stick moves, else a swing, a kick or a low strike at what is short, the
 * strong one making the slow swing, or a slow swing at anything within reach mid-chain. */
PlayerAnimator::Action PlayerAnimator::meleeRequest(PlayerDeed deed, bool moved) const {
    if (!deedHas(deed, deed_trait::kClose)) {
        return Action::Ready;
    }
    const bool automatic = deedHas(deed, deed_trait::kAutomatic);
    const bool strong = deedHas(deed, deed_trait::kStrong);
    const bool quick = !strong;
    const bool low = deedHas(deed, deed_trait::kLow);
    const bool lunge = m_melee.range == MeleeRange::Step && !low && moved && !automatic;
    if (strong && m_chain != 0 && moved && m_melee.range != MeleeRange::Beyond) {
        return Action::SlowStart;
    }
    if (lunge && playable(Action::Step1)) {
        return Action::Step1;
    }
    if (quick) {
        return low ? Action::LowKick : Action::Quick1;
    }
    return low ? Action::Low1 : Action::SlowStart;
}

bool PlayerAnimator::canAutoMelee() const {
    if (!bound() || entering() || m_quickPress || m_strongPress) {
        return false;
    }
    return (info(m_current).traits & action_trait::kAutoMelee) != 0;
}

/** What follows a quick or stepping swing: a buffered strong press makes the chain's power
 * swing; a press or a held button swings on, stepping at what is a pace away; else the
 * swing recovers. */
PlayerAnimator::Action PlayerAnimator::chainAfter(Action swing) const {
    if (buffered()) {
        if (const Action power = powerOf(m_chain); playable(power)) {
            return power;
        }
    }
    const bool second = swing == Action::Quick2 || swing == Action::Step2;
    const bool asked = m_quickPress || m_strongPress || m_quickHeld || m_strongHeld;
    if (asked && m_melee.range == MeleeRange::Step &&
        playable(second ? Action::Step3 : Action::Step2)) {
        return second ? Action::Step3 : Action::Step2;
    }
    if (asked && m_melee.range == MeleeRange::Swing) {
        return second ? Action::Quick3 : Action::Quick2;
    }
    return info(swing).next;
}

/** A swing turned the way its target lies, cut in from a walk, or made low at something
 * short (AnimAction's refinement pass). */
PlayerAnimator::Action PlayerAnimator::refine(Action action) const {
    Action refined = action;
    const f32 yaw = m_melee.yaw;
    if (action == Action::Quick1 || action == Action::Quick3 || action == Action::Step3 ||
        action == Action::Quick2 || action == Action::Step2) {
        const bool second = action == Action::Quick2 || action == Action::Step2;
        if (yaw > kBehind) {
            refined = second ? Action::Turn2 : Action::Turn;
        } else if (yaw < -kBehind) {
            refined = second ? Action::TurnLeft2 : Action::TurnLeft;
        } else if (yaw > kAside) {
            refined = second ? Action::Right2 : Action::Right;
        } else if (yaw < -kAside) {
            refined = second ? Action::Left2 : Action::Left;
        }
    } else if (action == Action::Step1 &&
               (m_current == Action::Walk2 || m_current == Action::Run2)) {
        refined = Action::WalkStrike;
    } else if (action == Action::PowerClose && m_melee.low) {
        refined = Action::PowerLow;
    }
    return playable(refined) ? refined : action;
}

/** The power swing a buffered strong press makes, by how far into the chain it comes. */
PlayerAnimator::Action PlayerAnimator::powerOf(s32 chain) {
    constexpr s32 kSpinChain = 3;
    if (chain >= kSpinChain) {
        return Action::Spin;
    }
    return chain == 1 ? Action::PowerClose : Action::PowerMed;
}

bool PlayerAnimator::playable(Action action) const {
    if (action == Action::PowerLow) {
        return m_sequences[index(action)] >= 0 || m_sequences[index(Action::PowerClose)] >= 0;
    }
    return m_sequences[index(action)] >= 0;
}

void PlayerAnimator::setCharacter(s32 character) {
    m_character = baseClassOf(character);
}

f32 PlayerAnimator::animationDuration(const Decision& decision) const {
    // DoPlayerAction 800AD280..800AD370 uses the requested attack type,
    // not the old action or selected follow-up. Types 1 and >=11 ignore both
    // items; a selected combo also stays unscaled when its request is READY.
    const AttackType request = info(decision.requested).attackType;
    if (info(decision.action).attackType == AttackType::Combo || request == AttackType::Combo ||
        request == AttackType::Guard || request == AttackType::Special) {
        return 1.0f;
    }
    // Rapid Fire accepts only types 9 and 10 (ordinary/strong throws and
    // gauntlets), not the shooting strafes in type 7. Speed is the fallback,
    // so owning both items never compounds their 0.75 duration multiplier.
    const bool rapidAction = request == AttackType::Throw || request == AttackType::StrongThrow;
    constexpr f32 kItemAnimationDuration = 0.75f;
    if (m_rapid && rapidAction) {
        return kItemAnimationDuration;
    }
    if (decision.action == Action::Defend) {
        // 800AD318..800AD34C: armor determines DEFEND2's duration, before
        // Speed's fallback. Armor is the scaled stat, not the displayed 0..999.
        return std::max(0.25f, 0.2f * m_guardArmor);
    }
    return m_speed ? kItemAnimationDuration : 1.0f;
}

void PlayerAnimator::play(const Decision& decision, f32 seconds) {
    const u32 target = sequenceOf(decision.action);
    // MBEndFrame (800B6ED8) waits at least 34 ms between native game updates.
    // InitAnim discards a finished phase's remainder, so polling those phase
    // transitions at 60 Hz inadvertently increases sustained firing speed.
    // Retain nominal two-hardware-tick shot boundaries, while still sampling
    // poses and accepting input interrupts on every update. No carry is kept
    // when a new phase starts, matching InitAnim rather than speeding the loop.
    constexpr f64 kShotHz = 30.0;
    constexpr f64 kTimeSlack = 0.000001;
    const auto before = std::floor((m_phaseSeconds + kTimeSlack) * kShotHz);
    m_phaseSeconds += std::max(0.0f, seconds);
    const bool shotBoundary = std::floor((m_phaseSeconds + kTimeSlack) * kShotHz) > before;
    // InitAnim caches animscale in seqscale at sequence start. Advance the
    // old sequence and any blend using real elapsed time, not a new item's
    // multiplier; CalcAnimInfo's smooth/snap test uses that cached period.
    m_player.advance(seconds, decision.repeat);
    if (shotBoundary) {
        // decide() has already tested the previous boundary's frame, just as
        // DoPlayerAction tests the old frame before calling AnimateTree.
        m_shotFrame = m_player.frame();
    }
    const bool done = m_player.finished();
    const bool different = !m_player.playing() || m_player.sequence() != target;
    bool restart = false;
    switch (decision.cut) {
    case Cut::WhenDoneIfDifferent: restart = done && different; break;
    case Cut::WhenDone: restart = done; break;
    case Cut::IfDifferent: restart = done || different; break;
    case Cut::Now: restart = true; break;
    }
    if (!restart || (decision.shotBoundary && !shotBoundary)) {
        return;
    }
    // action.c emits breath at entry, but hammer and gauntlets at the
    // completed wind-up/recovery boundary. Interrupted wind-ups never fire.
    if (decision.action == Action::Breathe) {
        m_itemReleased = PlayerDeed::Breathe;
    } else if (done && m_current == Action::Hammer && decision.action == Action::HammerRecover) {
        m_itemReleased = PlayerDeed::Hammer;
    } else if (done && m_current == Action::FireLeft &&
               decision.action == Action::FireLeftRecover) {
        m_itemReleased = PlayerDeed::FireLeft;
    } else if (done && m_current == Action::FireRight &&
               decision.action == Action::FireRightRecover) {
        m_itemReleased = PlayerDeed::FireRight;
    }
    // A fidget giving way starts the timers for the next one.
    if (m_current == Action::Idle1) {
        m_stillTicks = 0;
        m_fidgetTicks = 1;
    } else if (m_current == Action::Idle2Loop && decision.action != Action::Idle2Loop) {
        m_stillTicks = 0;
        m_fidgetTicks = 0;
    }
    // A walk, run or strafing half cycle giving way is a foot coming down: a strafing step
    // sounds like a walk's.
    if (const Foot foot = info(m_current).foot; foot != Foot::None) {
        m_footfall = foot;
    }
    // Both ordinary releases and strafing half-cycles let fly when they give way,
    // including the last moving shot when the strafe modifier is released.
    if (m_current == Action::ThrowRelease || m_current == Action::ThrowMovingRelease ||
        info(m_current).family == ActionFamily::StrafeShoot) {
        m_released = true;
    }
    // Contacts belong to completed swings, never to an interruption by damage or death.
    if (done && decision.action != Action::Death && decision.action != Action::HitReact &&
        decision.action != Action::Stun && decision.action != Action::SpikeHit &&
        decision.action != Action::FallBack && decision.action != Action::FallForward &&
        decision.action != Action::Whirled && decision.action != Action::Grabbed &&
        decision.action != Action::WebReact) {
        m_meleeBlow = info(m_current).blow;
    }
    // A strike beginning counts into the chain when a press came since the last one, and
    // starts the chain over when none did; either way the presses are spent.
    const ActionInfo& decided = info(decision.action);
    if ((decided.blow != MeleeBlow::None && decision.action != Action::SlowSwing) ||
        decision.action == Action::SlowStart) {
        m_chain = m_quickPress || m_strongPress ? m_chain + 1 : 0;
        m_quickPress = false;
        m_strongPress = false;
    } else if ((decided.traits & action_trait::kKeepsPress) == 0) {
        // Wind-ups and ordinary locomotion consume old edges; recoveries and
        // the native hold states preserve them for the next attack request.
        m_quickPress = false;
        m_strongPress = false;
    }
    // The spin and the power swings play their class's strong-attack rows.
    if (decision.action == Action::Spin || decision.action == Action::PowerClose ||
        decision.action == Action::PowerMed || decision.action == Action::PowerLow) {
        m_turboBegan = true;
    }
    // A legend item leaves the hand where a potion's magic would go off, the strong throw's
    // weapon would fly or the special shot's wind-up ends.
    const bool windUpOver =
        (m_current == Action::StrongThrow && decision.action == Action::StrongThrowRecover) ||
        (m_current == Action::SpecialShot && decision.action == Action::SpecialShotRecover) ||
        decision.action == Action::UsePotionRelease;
    if (m_legendAsked && windUpOver) {
        m_legendReleased = true;
    }
    if (!m_legendAsked && done &&
        (m_current == Action::SpecialShot || m_current == Action::SpecialShotRepeat) &&
        (decision.action == Action::SpecialShotRepeat ||
         decision.action == Action::SpecialShotRecover)) {
        m_superReleased = true;
    }
    if (m_current == Action::StrongThrow && decision.action == Action::StrongThrowRecover &&
        !m_legendAsked && m_character != kSorceressClass) {
        m_strongReleased = true;
    }
    if (m_legendAsked && decision.action != Action::UsePotion &&
        decision.action != Action::UsePotionRelease && decision.action != Action::StrongThrow &&
        decision.action != Action::StrongThrowRecover && decision.action != Action::SpecialShot &&
        decision.action != Action::SpecialShotRecover) {
        m_legendAsked = false; // the gesture is over
    }
    if (decision.action == Action::UsePotion || decision.action == Action::ThrowPotion) {
        m_potionLatch = true;
    }
    if (decision.action == Action::ThrowPotion) {
        m_potionThrowTicks = 0;
        m_potionThrowReleased = false;
    }
    m_potionUsed = decision.action == Action::UsePotionRelease && !m_shieldAsked && !m_legendAsked;
    m_potionShielded = decision.action == Action::UsePotionRelease && m_shieldAsked;
    m_potionThrown = decision.action == Action::ThrowPotionRelease;
    if ((decision.action == Action::Throw || decision.action == Action::ThrowMoving) &&
        !isThrow(m_current)) {
        m_attackSeconds = 0.0f;
    }
    m_previous = m_pose;
    m_player.setSpeed(1.0f / animationDuration(decision));
    // InitAnim dates a nonzero blend one base tick before its first sample.
    // Credit only that blend; the target's gameplay frame still starts at zero.
    m_player.start(m_tree->sequences[target], target, decision.transition, decision.startFrame,
                   AnimationPlayer::kTick);
    m_phaseSeconds = 0.0;
    m_shotFrame = m_player.frame();
    if (decision.action == Action::Start) {
        m_entered = true;
    }
    m_current = decision.action;
}

} // namespace gdl::game
