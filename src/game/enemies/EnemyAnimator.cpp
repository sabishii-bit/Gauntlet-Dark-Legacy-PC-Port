#include "game/enemies/EnemyAnimator.h"

namespace gdl::game {

namespace {

constexpr usize index(EnemyAction action) {
    return static_cast<usize>(action);
}

constexpr bool isHit(EnemyAction action) {
    return action >= EnemyAction::HitReact1;
}

} // namespace

bool EnemyAnimator::bind(const TreeInfo& tree, bool walksIn) {
    unbind();
    for (usize a = 0; a < kEnemyActionCount; ++a) {
        const auto sequence = tree.findSequence(kSequenceNames[a]);
        m_sequences[a] = sequence.has_value() ? static_cast<s32>(*sequence) : -1;
    }
    if (m_sequences[index(Action::Ready)] < 0) {
        return false;
    }
    m_tree = &tree;
    m_walksIn = walksIn;
    m_current = has(Action::Start) ? Action::Start : Action::Ready;
    const u32 first = sequenceOf(m_current);
    m_player.start(tree.sequences[first], first);
    m_pose.evaluate(tree, first, 0.0f);
    m_previous = m_pose;
    return true;
}

void EnemyAnimator::unbind() {
    m_tree = nullptr;
    m_sequences.fill(-1);
    m_current = Action::Ready;
    m_requested = Action::Ready;
    m_walksIn = false;
    m_struck = false;
    m_powerStruck = false;
    m_threw = false;
    m_dead = false;
    m_idleSeconds = 0.0f;
    m_player.stop();
}

void EnemyAnimator::request(Action action) {
    const bool attack = action >= Action::Attack && action <= Action::Attack5Recover;
    const bool throwing = action >= Action::Throw && action <= Action::ThrowFinish;
    if ((attack || throwing) && m_idleSeconds > 0.0f) {
        return;
    }
    if (kPriorities[index(m_requested)] >= kPriorities[index(action)]) {
        return;
    }
    m_requested = action;
}

u32 EnemyAnimator::sequenceOf(Action action) const {
    const s32 sequence = m_sequences[index(action)];
    return sequence >= 0 ? static_cast<u32>(sequence)
                         : static_cast<u32>(m_sequences[index(Action::Ready)]);
}

void EnemyAnimator::update(s32 ticks, f32 seconds, bool contact) {
    m_struck = false;
    m_powerStruck = false;
    m_threw = false;
    if (!bound() || m_dead) {
        m_requested = Action::Ready;
        return;
    }
    m_idleSeconds = std::max(m_idleSeconds - seconds, 0.0f);
    (void)ticks;
    play(decide(m_requested, contact), seconds);
    m_requested = Action::Ready;
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    if (m_player.transitioning()) {
        m_pose.blend(m_previous, m_player.transition());
    }
}

EnemyAnimator::Decision EnemyAnimator::decide(Action next, bool contact) const {
    Decision d;
    d.action = next;
    // A hit is answered from the stance, whatever the body was doing.
    const Action current = isHit(next) ? Action::Ready : m_current;
    const auto whenDone = [&d] {
        d.cut = Cut::WhenDoneIfDifferent;
        d.repeat = false;
    };
    const auto cutForHit = [&] {
        whenDone();
        if (isHit(next)) {
            d.cut = Cut::IfDifferent;
        }
    };
    switch (current) {
    case Action::Start:
        whenDone();
        if (m_walksIn) {
            d.action = has(Action::Walk) ? Action::Walk : Action::Run;
        }
        break;
    case Action::Ready:
        if (next == Action::Walk && has(Action::ReadyToWalk)) {
            d.action = Action::ReadyToWalk;
            d.cut = Cut::WhenDoneIfDifferent;
        } else if (next == Action::Run && has(Action::ReadyToRun)) {
            d.action = Action::ReadyToRun;
            d.cut = Cut::WhenDoneIfDifferent;
        } else if (next == Action::Attack || next == Action::Attack2 ||
                   next == Action::PowerAttack) {
            d.cut = Cut::WhenDoneIfDifferent;
        }
        break;
    case Action::ReadyToRun:
        cutForHit();
        if (!isHit(next)) {
            d.action = has(Action::Run) ? Action::Run : Action::Walk;
        }
        break;
    case Action::ReadyToWalk:
        cutForHit();
        if (!isHit(next)) {
            d.action = has(Action::Walk) ? Action::Walk : Action::Run;
        }
        break;
    case Action::WalkToReady:
    case Action::RunToReady:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::Ready;
        }
        break;
    case Action::Walk:
    case Action::Fly:
    case Action::Hover:
    case Action::Landing:
        if (next == Action::Ready) {
            if (has(Action::WalkToReady)) {
                d.action = Action::WalkToReady;
            }
            d.cut = Cut::WhenDoneIfDifferent;
        }
        break;
    case Action::Run:
        if (next == Action::Ready && has(Action::RunToReady)) {
            d.action = Action::RunToReady;
            d.cut = Cut::WhenDoneIfDifferent;
        }
        break;
    case Action::HitReact1:
    case Action::HitReact3:
    case Action::Dying:
    case Action::GetUp:
    case Action::Taunt:
        whenDone();
        break;
    case Action::HitReact2:
        whenDone();
        if (has(Action::GetUp)) {
            d.action = Action::GetUp;
        }
        break;
    case Action::Attack:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::AttackRecover;
        }
        break;
    case Action::AttackRecover:
        cutForHit();
        if (!isHit(next)) {
            if (has(Action::Attack2)) {
                d.action = Action::Attack2;
            } else if (next == Action::Attack) {
                d.action = Action::Attack;
            }
        }
        break;
    case Action::Attack2:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::Attack2Recover;
        }
        break;
    case Action::Attack2Recover:
        cutForHit();
        if (!isHit(next) && next == Action::Attack) {
            d.action = Action::Attack;
        }
        break;
    case Action::PowerAttack:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::PowerAttackRecover;
        }
        break;
    case Action::PowerAttackRecover:
        cutForHit();
        break;
    case Action::Attack4:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::Attack4Recover;
        }
        break;
    case Action::Attack5:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::Attack5Recover;
        }
        break;
    case Action::Attack4Recover:
    case Action::Attack5Recover:
        if (next == Action::Ready && contact) {
            d.action = Action::Attack;
        }
        break;
    case Action::RunAttack:
        cutForHit();
        if (!isHit(next) && next == Action::RunAttack) {
            d.action = Action::RunAttack2;
        }
        break;
    case Action::RunAttack2:
        cutForHit();
        if (!isHit(next) && next == Action::RunAttack) {
            d.action = Action::RunAttack;
        }
        break;
    case Action::Throw:
    case Action::Throw2:
        cutForHit();
        if (!isHit(next)) {
            d.action = Action::ThrowFinish;
        }
        break;
    case Action::ThrowFinish:
        cutForHit();
        if (!isHit(next)) {
            if (next == Action::Throw) {
                d.action = Action::Throw2;
            } else if (next == Action::Ready) {
                d.action = Action::ThrowToReady;
            }
        }
        break;
    case Action::ThrowToReady:
        cutForHit();
        d.action = Action::Ready;
        break;
    default:
        break;
    }
    // What the tree lacks is stood in for.
    switch (d.action) {
    case Action::Attack2:
    case Action::PowerAttack:
    case Action::Attack4:
    case Action::Attack5:
        if (!has(d.action)) {
            d.action = Action::Attack;
        }
        break;
    case Action::Walk:
    case Action::ReadyToRun:
        if (!has(d.action)) {
            d.action = Action::Run;
        }
        break;
    case Action::Run:
    case Action::ReadyToWalk:
        if (!has(d.action)) {
            d.action = Action::Walk;
        }
        break;
    case Action::WalkToReady:
    case Action::RunToReady:
    case Action::ThrowToReady:
        if (!has(d.action)) {
            d.action = Action::Ready;
        }
        break;
    case Action::Throw2:
        if (!has(d.action)) {
            d.action = Action::Throw;
        }
        break;
    case Action::Throw:
        if (!has(d.action)) {
            d.action = Action::Throw2;
        }
        break;
    default:
        break;
    }
    return d;
}

void EnemyAnimator::play(Decision decision, f32 seconds) {
    // A death the tree has no sequence for is played as being knocked down; anything else it
    // lacks is the stance, looping, and gives way at once.
    u32 target = sequenceOf(decision.action);
    if (!has(decision.action)) {
        if (decision.action == Action::Dying && has(Action::HitReact2)) {
            target = sequenceOf(Action::HitReact2);
        } else {
            decision.repeat = true;
            if (decision.cut != Cut::WhenDoneIfDifferent) {
                decision.cut = Cut::IfDifferent;
            }
        }
    }
    m_player.advance(seconds, decision.repeat);
    const bool done = m_player.finished();
    // A death played out is the end: nothing follows it.
    if (m_current == Action::Dying && done) {
        m_dead = true;
        return;
    }
    const bool different = !m_player.playing() || m_player.sequence() != target;
    const bool restart = decision.cut == Cut::IfDifferent ? (done || different) : (done && different);
    if (!restart) {
        return;
    }
    const Action was = m_current;
    const Action now = decision.action;
    const bool swing = was == Action::Attack || was == Action::Attack2 ||
                       was == Action::Attack4 || was == Action::Attack5;
    const bool recover = now == Action::AttackRecover || now == Action::Attack2Recover ||
                         now == Action::Attack4Recover || now == Action::Attack5Recover;
    m_struck = swing && recover;
    m_powerStruck = was == Action::PowerAttack && now == Action::PowerAttackRecover;
    m_threw = ((was == Action::Throw || was == Action::Throw2) && now == Action::ThrowFinish) ||
              (was == Action::RunAttack && now == Action::RunAttack2);
    m_previous = m_pose;
    m_player.start(m_tree->sequences[target], target);
    m_current = now;
}

} // namespace gdl::game
