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
    m_player.stop();
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

void PlayerAnimator::update(PlayerMotion motion, s32 ticks, f32 seconds) {
    if (!bound()) {
        return;
    }
    m_footfall = Foot::None;
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
    m_previous = m_pose;
    m_player.start(m_tree->sequences[target], target, decision.transition);
    if (decision.action == Action::Start) {
        m_entered = true;
    }
    m_current = decision.action;
}

} // namespace gdl::game
