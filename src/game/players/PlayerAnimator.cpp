#include "game/players/PlayerAnimator.h"

namespace gdl::game {

namespace {

constexpr usize index(PlayerAnimator::Action action) {
    return static_cast<usize>(action);
}

} // namespace

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
    m_potionUsed = false;
    m_potionThrown = false;
    m_potionLatch = false;
    m_dead = false;
    m_turboBegan = false;
    m_attackSeconds = 0.0f;
    m_player.stop();
}

PlayerAnimator::Action PlayerAnimator::turboActionOf(PlayerDeed deed) {
    switch (deed) {
    case PlayerDeed::TurboStrong: return Action::TurboStrong;
    case PlayerDeed::TurboFull: return Action::TurboFull;
    case PlayerDeed::Shove: return Action::Shove;
    default: return Action::Ready;
    }
}

bool PlayerAnimator::canBegin(PlayerDeed deed) const {
    const Action action = turboActionOf(deed);
    if (action == Action::Ready) {
        return false;
    }
    return bound() && m_sequences[index(action)] >= 0 && m_entered && !throwing() &&
           !conjuring() && !reacting() && !turboing() && !dying();
}

PlayerMotion PlayerAnimator::motionFor(f32 stickMagnitude) {
    if (stickMagnitude > kRunMagnitude) {
        return PlayerMotion::Run;
    }
    return stickMagnitude > 0.0f ? PlayerMotion::Walk : PlayerMotion::Stand;
}

u32 PlayerAnimator::sequenceOf(Action action) const {
    const s32 sequence = m_sequences[index(action)];
    return sequence >= 0 ? static_cast<u32>(sequence)
                         : static_cast<u32>(m_sequences[index(Action::Ready)]);
}

void PlayerAnimator::update(PlayerMotion motion, s32 ticks, f32 seconds, PlayerDeed deed) {
    if (!bound()) {
        return;
    }
    m_footfall = Foot::None;
    m_released = false;
    m_potionUsed = false;
    m_potionThrown = false;
    m_turboBegan = false;
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
    // A hit cuts into anything at once, unless one is already being reeled from; while it
    // plays nothing else is asked of the body.
    const bool struck = deed == PlayerDeed::Flinch || deed == PlayerDeed::Reel;
    if (struck && !reacting()) {
        const Action reaction = deed == PlayerDeed::Flinch ? Action::HitReact : Action::Stun;
        if (m_sequences[index(reaction)] >= 0) {
            Decision reel;
            reel.action = reaction;
            reel.cut = Cut::Now;
            play(reel, seconds);
            m_released = false; // a throw it cut into never leaves the hand
            m_potionUsed = false;
            m_potionThrown = false;
            m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
            return;
        }
    }
    // A turbo move cuts into standing, walking and running at once and plays through.
    if (canBegin(deed)) {
        Decision move;
        move.action = turboActionOf(deed);
        move.cut = Cut::Now;
        play(move, seconds);
        m_turboBegan = true;
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        return;
    }
    // The guard: up at once when asked for, held for as long as it is, then let down. A
    // class without the sequences does not guard.
    const bool free = m_entered && !throwing() && !conjuring() && !reacting() && !turboing();
    const bool asked = deed == PlayerDeed::Defend && free &&
                       m_sequences[index(Action::Defend)] >= 0;
    if (asked || guarding()) {
        Decision guard;
        if (asked && (!guarding() || m_current == Action::DefendLower)) {
            guard.action = m_sequences[index(Action::DefendRaise)] >= 0 ? Action::DefendRaise
                                                                        : Action::Defend;
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
                            deed == PlayerDeed::Shove;
    if (reacting() || struck || turboing() || turboAsked) {
        deed = PlayerDeed::None;
        motion = PlayerMotion::Stand;
    }
    bool attack = deed == PlayerDeed::Attack;
    // A class without a deed's sequences does not do it.
    // One potion a press: the button must come up before it asks for another.
    if (deed != PlayerDeed::UsePotion && deed != PlayerDeed::ThrowPotion) {
        m_potionLatch = false;
    }
    const bool use = deed == PlayerDeed::UsePotion && !m_potionLatch &&
                     m_sequences[index(Action::UsePotion)] >= 0;
    const bool toss = deed == PlayerDeed::ThrowPotion && !m_potionLatch &&
                      m_sequences[index(Action::ThrowPotion)] >= 0;
    if (throwing()) {
        m_attackSeconds += seconds;
    }
    // A class without the throw's sequences does not throw.
    attack = attack && m_sequences[index(Action::Throw)] >= 0;
    if (attack || throwing() || use || toss || conjuring()) {
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
    } else if (attack) {
        requested = Action::Throw;
    } else if (motion == PlayerMotion::Run) {
        requested = Action::Run1;
    } else if (motion == PlayerMotion::Walk) {
        requested = Action::Walk1;
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
    case Action::Start:
        break;
    case Action::Throw:
    case Action::ThrowMoving:
        // The wind-up gives way to the release at its end, or at once from its second frame.
        d.action = m_current == Action::Throw ? Action::ThrowRelease : Action::ThrowMovingRelease;
        d.cut = m_player.frame() >= kReleaseFrame ? Cut::IfDifferent : Cut::WhenDoneIfDifferent;
        break;
    case Action::ThrowRelease:
        d.action = Action::ThrowRecover;
        break;
    case Action::ThrowMovingRelease:
        d.action = Action::ThrowMovingRecover;
        break;
    case Action::ThrowRecover:
    case Action::ThrowMovingRecover:
        break; // whatever is asked next, once recovered
    case Action::UsePotion:
        d.action = Action::UsePotionRelease;
        break;
    case Action::ThrowPotion:
        d.action = Action::ThrowPotionRelease;
        break;
    case Action::UsePotionRelease:
    case Action::ThrowPotionRelease:
    case Action::Death:
    case Action::HitReact:
    case Action::Stun:
    case Action::TurboStrong:
    case Action::TurboFull:
    case Action::Shove:
    case Action::DefendRaise:
    case Action::Defend:
    case Action::DefendLower:
        break; // whatever is asked next, once let go
    }
    // A potion cuts into standing, walking and running at once, as an attack does.
    if ((requested == Action::UsePotion || requested == Action::ThrowPotion) &&
        d.action == requested && !isThrow(m_current) && !conjuring() &&
        m_current != Action::Start) {
        d.cut = Cut::IfDifferent;
    }
    // An attack cuts into walking and running at once, and from their first halves takes the
    // moving wind-up.
    if (requested == Action::Throw && d.action == Action::Throw && !isThrow(m_current)) {
        if (m_current != Action::Start) {
            d.cut = Cut::IfDifferent;
        }
        if (m_current == Action::Walk1 || m_current == Action::Run1) {
            d.action = Action::ThrowMoving;
        }
    }
    if (d.action == Action::Ready && m_current != Action::Ready) {
        d.transition = kStanceBlend;
    }
    return d;
}

void PlayerAnimator::play(const Decision& decision, f32 seconds) {
    const u32 target = sequenceOf(decision.action);
    m_player.advance(seconds, decision.repeat);
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
    if (m_current == Action::ThrowRelease || m_current == Action::ThrowMovingRelease) {
        m_released = true;
    }
    if (decision.action == Action::UsePotion || decision.action == Action::ThrowPotion) {
        m_potionLatch = true;
    }
    m_potionUsed = decision.action == Action::UsePotionRelease;
    m_potionThrown = decision.action == Action::ThrowPotionRelease;
    if ((decision.action == Action::Throw || decision.action == Action::ThrowMoving) &&
        !isThrow(m_current)) {
        m_attackSeconds = 0.0f;
    }
    m_previous = m_pose;
    m_player.start(m_tree->sequences[target], target, decision.transition);
    if (decision.action == Action::Start) {
        m_entered = true;
    }
    m_current = decision.action;
}

} // namespace gdl::game
