#include "game/players/PlayerAnimator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"

namespace gdl::game {

namespace {

constexpr usize index(PlayerAnimator::Action action) {
    return static_cast<usize>(action);
}

// The classes whose power swings keep their feet, in ClassData's table order.
constexpr s32 kWizardClass = 2;
constexpr s32 kArcherClass = 3;
constexpr s32 kKnightClass = 5;
constexpr s32 kSorceressClass = 6;
constexpr s32 kJesterClass = 7;

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

/** Whether the deed is a legend item's gesture. */
constexpr bool isLegend(PlayerDeed deed) {
    return deed == PlayerDeed::HurlLegend || deed == PlayerDeed::ThrowLegend ||
           deed == PlayerDeed::ShootLegend;
}

} // namespace

bool PlayerAnimator::isBlockableAttack(Action action) {
    // PlayerAttacking(level 1): categories 2, 5, 10, and 11 or higher.
    // Recoveries retain their attack category; quick, step and low swings do not qualify.
    switch (action) {
    case Action::SlowStart:
    case Action::SlowSwing:
    case Action::SlowRecover:
    case Action::PowerClose:
    case Action::PowerCloseRecover:
    case Action::PowerMed:
    case Action::PowerMedRecover:
    case Action::Spin:
    case Action::SpinRecover:
    case Action::StrongThrow:
    case Action::StrongThrowRecover:
    case Action::FireLeft:
    case Action::FireLeftRecover:
    case Action::FireRight:
    case Action::FireRightRecover:
    case Action::TurboStrong:
    case Action::TurboFull:
    case Action::SpecialShot:
    case Action::SpecialShotRecover:
    case Action::SpecialShotRepeat:
    case Action::Hammer:
    case Action::HammerRecover:
    case Action::Breathe:
    case Action::BreatheRecover: return true;
    default: return action >= Action::ComboAct1 && action <= Action::ComboJes;
    }
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
    m_strafe = StrafeWay::None;
    m_comboClass = -1;
    m_comboRide = false;
    m_attackSeconds = 0.0f;
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

PlayerAnimator::Action PlayerAnimator::comboHeldActionOf(s32 grabberClass) {
    switch (grabberClass) {
    case 0: return Action::ComboWar1;
    case 1: return Action::ComboVal;
    case 2: return Action::ComboWiz;
    case 3: return Action::ComboArc;
    case 4: return Action::ComboDwf1;
    case 5: return Action::ComboKni;
    case 6: return Action::ComboSor;
    case 7: return Action::ComboJes;
    default: return Action::Ready;
    }
}

PlayerAnimator::Action PlayerAnimator::comboThrownActionOf(s32 grabberClass) {
    switch (grabberClass) {
    case 0: return Action::ComboWar2;
    case 4: return Action::ComboDwf2;
    default: return Action::Ready;
    }
}

PlayerAnimator::Action PlayerAnimator::strafeStep(StrafeWay way, bool shooting) {
    const usize base = index(shooting ? Action::StrafeShootForward1 : Action::StrafeForward1);
    const usize steps = way == StrafeWay::None ? 0 : static_cast<usize>(way) - 1;
    return static_cast<Action>(base + steps * 2);
}

PlayerAnimator::Action PlayerAnimator::firstHalfOf(Action step) {
    const usize base = index(Action::StrafeForward1);
    return static_cast<Action>(base + (index(step) - base) / 2 * 2);
}

PlayerAnimator::Action PlayerAnimator::otherHalfOf(Action step) {
    const usize base = index(Action::StrafeForward1);
    const usize offset = index(step) - base;
    return static_cast<Action>(base + (offset % 2 == 0 ? offset + 1 : offset - 1));
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

bool PlayerAnimator::canBegin(PlayerDeed deed) const {
    const Action action = turboActionOf(deed);
    if (action == Action::Ready) {
        return false;
    }
    // A turbo move cuts a close attack off; nothing else does.
    const bool cutsMelee = deed == PlayerDeed::TurboStrong || deed == PlayerDeed::TurboFull ||
                           deed == PlayerDeed::Shove || deed == PlayerDeed::Combo;
    return bound() && m_sequences[index(action)] >= 0 && !entering() && !throwing() &&
           (!meleeing() || cutsMelee) && !conjuring() && !reacting() && !turboing() &&
           !comboBound() && !dying();
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

void PlayerAnimator::update(PlayerMotion motion, s32 ticks, f32 seconds, PlayerDeed deed) {
    m_meleeBlow = MeleeBlow::None;
    if (!bound()) {
        return;
    }
    evaluatePresentation(m_presentationPrevious);
    m_presentationGeneration = m_player.generation();
    // A press is a button going down; it stays fresh until the next strike begins.
    const bool quickHeld =
        deed == PlayerDeed::Attack || deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow;
    const bool strongHeld = deed == PlayerDeed::StrongAttack || deed == PlayerDeed::MeleeSlow ||
                            deed == PlayerDeed::MeleeSlowLow;
    m_quickPress = m_quickPress || (quickHeld && !m_quickHeld);
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
        Decision fall;
        fall.action = Action::Death;
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
        Decision captured;
        captured.action = deed == PlayerDeed::Grabbed ? Action::Grabbed : Action::FallBack;
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
    const bool felled = deed == PlayerDeed::FallBack || deed == PlayerDeed::FallForward ||
                        deed == PlayerDeed::Whirled;
    const bool struck = deed == PlayerDeed::Flinch || deed == PlayerDeed::Reel ||
                        deed == PlayerDeed::Spike || deed == PlayerDeed::Webbed || felled;
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
            Decision reel;
            reel.action = reaction;
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
        Decision move;
        move.action = turboActionOf(deed);
        move.cut = Cut::Now;
        m_legendAsked = isLegend(deed);
        m_shieldAsked = false;
        play(move, seconds);
        m_turboBegan = !m_legendAsked && deed != PlayerDeed::SuperShot && !itemAttacking();
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        return;
    }
    // The guard: up at once when asked for, held for as long as it is, then let down. A
    // class without the sequences does not guard.
    // The guard also cuts into the recovery from a quick or stepping swing.
    const bool recovering =
        m_current == Action::Quick2Recover || m_current == Action::Quick3Recover ||
        m_current == Action::Step2Recover || m_current == Action::Step3Recover ||
        m_current == Action::WalkStrikeRecover;
    const bool free = !entering() && !throwing() && (!meleeing() || recovering) && !conjuring() &&
                      !reacting() && !turboing();
    const bool asked =
        deed == PlayerDeed::Defend && free && m_sequences[index(Action::Defend)] >= 0;
    if (asked || guarding()) {
        Decision guard;
        if (asked && (!guarding() || m_current == Action::DefendLower)) {
            guard.action =
                m_sequences[index(Action::DefendRaise)] >= 0 ? Action::DefendRaise : Action::Defend;
            guard.cut = Cut::Now;
        } else if (asked) {
            guard.action = Action::Defend;
            guard.repeat = m_current == Action::Defend;
            guard.cut = Cut::WhenDoneIfDifferent;
        } else if (m_current != Action::DefendLower &&
                   m_sequences[index(Action::DefendLower)] >= 0) {
            guard.action = Action::DefendLower;
            guard.cut = Cut::Now;
        } else {
            guard.action = Action::Ready;
            guard.cut = m_current == Action::DefendLower ? Cut::WhenDone : Cut::Now;
            guard.transition = kStanceBlend;
        }
        play(guard, seconds);
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        if (m_player.transitioning()) {
            m_pose.blend(m_previous, m_player.transition());
        }
        return;
    }
    const bool turboAsked = deed == PlayerDeed::TurboStrong || deed == PlayerDeed::TurboFull ||
                            deed == PlayerDeed::Shove || deed == PlayerDeed::StrongAttack ||
                            isLegend(deed);
    if (reacting() || struck || turboing() || turboAsked) {
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
    play(decide(requested), seconds);
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    if (m_player.transitioning()) {
        m_pose.blend(m_previous, m_player.transition());
    }
}

PlayerAnimator::Decision PlayerAnimator::decide(Action requested) const {
    Decision d;
    d.action = requested;
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
    case Action::Walk1:
        if (requested == Action::Walk1) {
            d.action = Action::Walk2;
        }
        break;
    case Action::Walk2:
        if (requested == Action::Walk1) {
            d.action = Action::Walk1;
        }
        break;
    case Action::Run1:
        if (requested == Action::Run1) {
            d.action = Action::Run2;
        }
        break;
    case Action::Run2:
        if (requested == Action::Run1) {
            d.action = Action::Run1;
        }
        break;
    case Action::Start: break;
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
    case Action::SlowStart: d.action = Action::SlowSwing; break;
    case Action::SlowSwing:
    case Action::Right:
    case Action::Right2:
    case Action::Left:
    case Action::Left2:
    case Action::Turn:
    case Action::Turn2:
    case Action::TurnLeft:
    case Action::TurnLeft2:
    case Action::PowerClose:
    case Action::PowerMed:
    case Action::PowerLow: d.action = recoveryOf(m_current); break;
    case Action::Low1:
    case Action::Low2:
        d.action = Action::LowRecover;
        if (requested == Action::Low1) {
            d.action = m_current == Action::Low1 ? Action::Low2 : Action::Low1;
        }
        break;
    case Action::LowRecover:
    case Action::SlowRecover:
    case Action::RightRecover:
    case Action::Right2Recover:
    case Action::LeftRecover:
    case Action::Left2Recover:
    case Action::TurnRecover:
    case Action::Turn2Recover:
    case Action::TurnLeftRecover:
    case Action::TurnLeft2Recover:
    case Action::SpinRecover:
    case Action::PowerCloseRecover:
    case Action::PowerMedRecover:
    case Action::PowerLowRecover: d.cut = Cut::WhenDone; break;
    case Action::Throw:
    case Action::ThrowMoving:
        // A close attack or moving shot takes over the wind-up at its current frame
        // (DoPlayerAction, P_THROW/P_THROWQ's non-throw attack branch).
        if (isMelee(requested) ||
            (requested >= Action::StrafeShootForward1 && requested <= Action::StrafeShootRight2)) {
            d.cut = Cut::IfDifferent;
            d.startFrame = std::round(m_player.frame());
            break;
        }
        // The wind-up gives way to the release at its end, or at once from its second frame.
        d.action = m_current == Action::Throw ? Action::ThrowRelease : Action::ThrowMovingRelease;
        d.cut = m_player.frame() >= kReleaseFrame ? Cut::IfDifferent : Cut::WhenDoneIfDifferent;
        break;
    case Action::ThrowRelease: d.action = Action::ThrowRecover; break;
    case Action::ThrowMovingRelease: d.action = Action::ThrowMovingRecover; break;
    case Action::ThrowRecover:
    case Action::ThrowMovingRecover: break; // whatever is asked next, once recovered
    case Action::UsePotion: d.action = Action::UsePotionRelease; break;
    case Action::ThrowPotion: d.action = Action::ThrowPotionRelease; break;
    case Action::UsePotionRelease:
    case Action::ThrowPotionRelease:
    case Action::Death:
    case Action::HitReact:
    case Action::SpikeHit:
    case Action::Stun:
    case Action::WebReact:
    case Action::TurboStrong:
    case Action::TurboFull:
    case Action::Shove:
    case Action::DefendRaise:
    case Action::Defend:
    case Action::DefendLower:
    case Action::StrongThrowRecover: break; // whatever is asked next, once let go
    case Action::StrongThrow:
        d.action = Action::StrongThrowRecover; // the weapon leaves as the wind-up ends
        break;
    case Action::SpecialShot:
    case Action::SpecialShotRepeat:
        d.action =
            !m_legendAsked && m_superHeld && m_sequences[index(Action::SpecialShotRepeat)] >= 0
                ? Action::SpecialShotRepeat
                : Action::SpecialShotRecover;
        d.cut = Cut::WhenDone;
        break;
    case Action::SpecialShotRecover: break;
    case Action::Hammer: d.action = Action::HammerRecover; break;
    case Action::Breathe: d.action = Action::BreatheRecover; break;
    case Action::FireLeft: d.action = Action::FireLeftRecover; break;
    case Action::FireRight: d.action = Action::FireRightRecover; break;
    case Action::HammerRecover:
    case Action::BreatheRecover:
    case Action::FireLeftRecover:
    case Action::FireRightRecover: break;
    case Action::FallBack:
    case Action::Whirled: d.action = Action::GetUpBack; break; // action.c 1270
    case Action::FallForward: d.action = Action::GetUpForward; break;
    case Action::GetUpBack:
    case Action::GetUpForward:
    case Action::Pick: break; // plays through, then whatever is asked
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
    case Action::DeathGrabRelease: break;
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
    case Action::ComboAct3:
    case Action::ComboWar1:
    case Action::ComboWar3:
    case Action::ComboVal:
    case Action::ComboWiz:
    case Action::ComboArc:
    case Action::ComboDwf1:
    case Action::ComboDwf3:
    case Action::ComboKni:
    case Action::ComboSor:
    case Action::ComboJes: break; // plays through and holds; whatever is asked follows
    default:
        // A strafing step gives way to its other half while the same way is kept.
        if (strafing() && requested == firstHalfOf(m_current)) {
            d.action = otherHalfOf(m_current);
        }
        // AnimAction's walking-strafe cases use mode 2 for an attack: it cuts in
        // immediately. Waiting for the footstep's end loses short button presses.
        // Shooting strafes still finish each half before releasing another weapon.
        if (m_current >= Action::StrafeForward1 && m_current <= Action::StrafeRight2 &&
            requested >= Action::StrafeShootForward1 && requested <= Action::StrafeShootRight2) {
            d.cut = Cut::IfDifferent;
        }
        break;
    }
    // A partner's combo takes the body at once (action.c 558's mode 2 for P_FALL_DOWN to
    // P_GRABBED), once and not looped.
    if (requested >= Action::ComboWar1 && requested <= Action::ComboJes && d.action == requested &&
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
        if (m_current != Action::Start && !meleeing() && !strafing() && !picking) {
            d.cut = Cut::IfDifferent;
        }
        if (m_current == Action::Walk1 || m_current == Action::Run1) {
            d.action = Action::ThrowMoving;
        }
    }
    if (isMelee(requested) && d.action == requested && !meleeing() && !throwing() && !conjuring() &&
        !reacting() && !turboing() && !entering() && !picking) {
        d.cut = Cut::IfDifferent;
    }
    // Death is taken hold of at once (P_DEATHGRAB's mode 2).
    if (requested == Action::DeathGrabStart && d.action == requested && !grabbingDeath() &&
        !entering() && !reacting()) {
        d.cut = Cut::IfDifferent;
    }
    if (d.action == Action::Ready && m_current != Action::Ready) {
        d.transition = kStanceBlend;
    }
    if (d.action != m_current) {
        d.action = refine(d.action);
    }
    return d;
}

/** The close attack a deed asks for (AnimAction's requests): a step into what is a pace
 * away while the stick moves, else a swing, a kick or a low strike at what is short, the
 * strong one making the slow swing, or a slow swing at anything within reach mid-chain. */
PlayerAnimator::Action PlayerAnimator::meleeRequest(PlayerDeed deed, bool moved) const {
    const bool quick = deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow;
    const bool strong = deed == PlayerDeed::MeleeSlow || deed == PlayerDeed::MeleeSlowLow;
    if (!quick && !strong) {
        return Action::Ready;
    }
    const bool low = deed == PlayerDeed::MeleeLow || deed == PlayerDeed::MeleeSlowLow;
    const bool lunge = m_melee.range == MeleeRange::Step && !low && moved;
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
    return recoveryOf(swing);
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

PlayerAnimator::Action PlayerAnimator::recoveryOf(Action swing) {
    switch (swing) {
    case Action::Quick2: return Action::Quick2Recover;
    case Action::Quick1:
    case Action::Quick3: return Action::Quick3Recover;
    case Action::Step2: return Action::Step2Recover;
    case Action::Step1:
    case Action::Step3: return Action::Step3Recover;
    case Action::WalkStrike: return Action::WalkStrikeRecover;
    case Action::SlowSwing: return Action::SlowRecover;
    case Action::LowKick: return Action::LowKickRecover;
    case Action::Low1:
    case Action::Low2: return Action::LowRecover;
    case Action::Right: return Action::RightRecover;
    case Action::Right2: return Action::Right2Recover;
    case Action::Left: return Action::LeftRecover;
    case Action::Left2: return Action::Left2Recover;
    case Action::Turn: return Action::TurnRecover;
    case Action::Turn2: return Action::Turn2Recover;
    case Action::TurnLeft: return Action::TurnLeftRecover;
    case Action::TurnLeft2: return Action::TurnLeft2Recover;
    case Action::Spin: return Action::SpinRecover;
    case Action::PowerClose: return Action::PowerCloseRecover;
    case Action::PowerMed: return Action::PowerMedRecover;
    case Action::PowerLow: return Action::PowerLowRecover;
    default: return Action::Ready;
    }
}

/** What a completed swing does: the quick, turned, spinning and low ones the character's
 * own harm; the slow swing and the steps twice it, knocking back; the kick knocking a short
 * body down; the power swings three times it, knocking down. */
MeleeBlow PlayerAnimator::blowOf(Action swing) {
    switch (swing) {
    case Action::Quick1:
    case Action::Quick2:
    case Action::Quick3:
    case Action::Right:
    case Action::Right2:
    case Action::Left:
    case Action::Left2:
    case Action::Turn:
    case Action::Turn2:
    case Action::TurnLeft:
    case Action::TurnLeft2:
    case Action::Spin:
    case Action::Low1:
    case Action::Low2: return MeleeBlow::Plain;
    case Action::SlowSwing:
    case Action::Step1:
    case Action::Step2:
    case Action::Step3:
    case Action::WalkStrike: return MeleeBlow::Heavy;
    case Action::LowKick: return MeleeBlow::Kick;
    case Action::PowerClose:
    case Action::PowerMed:
    case Action::PowerLow: return MeleeBlow::Power;
    default: return MeleeBlow::None;
    }
}

void PlayerAnimator::setCharacter(s32 character) {
    m_character =
        character >= 0 && character < kSumnerClass ? character % kStartingClassCount : character;
}

f32 PlayerAnimator::meleePace() const {
    const bool rooted = m_character == kKnightClass || m_character == kSorceressClass;
    switch (m_current) {
    case Action::SlowStart:
    case Action::SlowSwing:
    case Action::SlowRecover: return 0.0f;
    case Action::PowerClose:
    case Action::PowerCloseRecover:
    case Action::PowerMedRecover:
        if (rooted) {
            return 0.0f;
        }
        return m_character == kWizardClass ? kWizardPowerPace : kPowerPace;
    case Action::PowerMed:
        if (m_character == kJesterClass || m_character == kSorceressClass) {
            return 0.0f;
        }
        return m_character == kWizardClass || m_character == kArcherClass ? kWizardPowerPace
                                                                          : kPowerPace;
    case Action::Quick1:
    case Action::Quick2:
    case Action::Quick3:
    case Action::Quick2Recover:
    case Action::Quick3Recover: return kQuickMeleePace;
    case Action::Spin:
    case Action::SpinRecover: return kSpinPace;
    case Action::Step1:
    case Action::Step2:
    case Action::Step3:
    case Action::Step2Recover:
    case Action::Step3Recover:
    case Action::WalkStrike:
    case Action::WalkStrikeRecover: return m_moved ? 1.0f : kIdleStepPace;
    case Action::PowerLow:
    case Action::PowerLowRecover: return kPowerLowPace;
    default: return 1.0f; // the turned swings, the low strikes and the kick
    }
}

f32 PlayerAnimator::turnScale() const {
    constexpr f32 kChargerTurn = 0.5f; // COMBODWF2's turn scale (action.c 1804)
    constexpr f32 kFullTurboTurn = 0.25f;
    constexpr f32 kSpellStormLockFrame = 11;
    if (m_current == Action::ComboDwf2) {
        return kChargerTurn;
    }
    if (comboing() || comboBound()) {
        return 0.0f;
    }
    switch (m_current) {
    case Action::TurboFull:
        // DoPlayerAction's A4C: full moves turn slowly; Spell Storm fixes its
        // heading once the first knight's birth begins, strictly after frame 11.
        return m_character == kSorceressClass && m_player.frame() > kSpellStormLockFrame
                   ? 0.0f
                   : kFullTurboTurn;
    case Action::PowerClose:
    case Action::PowerCloseRecover:
    case Action::PowerMedRecover:
        return m_character == kKnightClass || m_character == kSorceressClass ? 0.0f : 1.0f;
    case Action::PowerMed:
        return m_character == kJesterClass || m_character == kSorceressClass ? 0.0f : 1.0f;
    case Action::Quick1:
    case Action::Quick2:
    case Action::Quick3:
    case Action::Quick2Recover:
    case Action::Quick3Recover: return 0.0f;
    case Action::Step1:
    case Action::Step2:
    case Action::Step3:
    case Action::Step2Recover:
    case Action::Step3Recover:
    case Action::WalkStrike:
    case Action::WalkStrikeRecover: return kStepTurn;
    default: return 1.0f;
    }
}

void PlayerAnimator::play(const Decision& decision, f32 seconds) {
    const u32 target = sequenceOf(decision.action);
    const bool rapidAction = isThrow(m_current) || (m_current >= Action::StrafeShootForward1 &&
                                                    m_current <= Action::StrafeShootRight2);
    const bool speedAction =
        m_speed && !entering() && !dying() && !reacting() && !turboing() && !conjuring();
    constexpr f32 kItemAnimationDuration = 0.75f;
    m_player.advance(seconds /
                         ((m_rapid && rapidAction) || speedAction ? kItemAnimationDuration : 1.0f),
                     decision.repeat);
    const bool done = m_player.finished();
    const bool different = !m_player.playing() || m_player.sequence() != target;
    bool restart = false;
    switch (decision.cut) {
    case Cut::WhenDoneIfDifferent: restart = done && different; break;
    case Cut::WhenDone: restart = done; break;
    case Cut::IfDifferent: restart = done || different; break;
    case Cut::Now: restart = true; break;
    }
    if (!restart) {
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
    // A walk or run half cycle giving way is a foot coming down.
    if (m_current == Action::Walk1 || m_current == Action::Run1) {
        m_footfall = Foot::First;
    } else if (m_current == Action::Walk2 || m_current == Action::Run2) {
        m_footfall = Foot::Second;
    }
    // Both ordinary releases and strafing half-cycles let fly when they give way,
    // including the last moving shot when the strafe modifier is released.
    if (m_current == Action::ThrowRelease || m_current == Action::ThrowMovingRelease ||
        (m_current >= Action::StrafeShootForward1 && m_current <= Action::StrafeShootRight2)) {
        m_released = true;
    }
    // Contacts belong to completed swings, never to an interruption by damage or death.
    if (done && decision.action != Action::Death && decision.action != Action::HitReact &&
        decision.action != Action::Stun && decision.action != Action::SpikeHit &&
        decision.action != Action::FallBack && decision.action != Action::FallForward &&
        decision.action != Action::Whirled && decision.action != Action::Grabbed &&
        decision.action != Action::WebReact) {
        m_meleeBlow = blowOf(m_current);
    }
    // A strike beginning counts into the chain when a press came since the last one, and
    // starts the chain over when none did; either way the presses are spent.
    if ((blowOf(decision.action) != MeleeBlow::None && decision.action != Action::SlowSwing) ||
        decision.action == Action::SlowStart) {
        m_chain = m_quickPress || m_strongPress ? m_chain + 1 : 0;
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
    // A strafing step sounds like a walk's.
    if (strafing()) {
        const bool first = (index(m_current) - index(Action::StrafeForward1)) % 2 == 0;
        m_footfall = first ? Foot::First : Foot::Second;
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
    m_player.start(m_tree->sequences[target], target, decision.transition, decision.startFrame);
    if (decision.action == Action::Start) {
        m_entered = true;
    }
    m_current = decision.action;
}

} // namespace gdl::game
