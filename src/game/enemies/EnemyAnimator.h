#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreePose.h"

namespace gdl::game {

/** What an enemy's body can be doing, in the original's order. */
enum class EnemyAction : std::uint8_t {
    Ready,
    Start,
    Taunt,
    Walk,
    Run,
    Fly,
    Hover,
    Landing,
    WalkToReady,
    ReadyToWalk,
    RunToReady,
    ReadyToRun,
    Attack, ///< the swing; the blow lands as its recovery begins
    AttackRecover,
    Attack2,
    Attack2Recover,
    PowerAttack, ///< every eighth blow, half as strong again
    PowerAttackRecover,
    Attack4,
    Attack4Recover,
    Attack5,
    Attack5Recover,
    RunAttack,
    RunAttack2,
    Throw,
    Throw2,
    ThrowFinish, ///< the missile leaves as it begins
    ThrowToReady,
    HitReact1, ///< a flinch
    HitReact2, ///< knocked down, then up again
    HitReact3,
    GetUp,
    Dying
};

inline constexpr std::size_t kEnemyActionCount = 33;

/**
 * Drives an enemy's body: which sequence plays, when one gives way to another, and when a
 * blow or a missile is let go. The mind asks for actions through the tick by priority, the
 * loudest winning; the body answers as the original's dispatcher does, chaining a swing into
 * its recovery and cutting anything short for a hit.
 */
class EnemyAnimator {
public:
    using Action = EnemyAction;

    static constexpr std::array<std::string_view, kEnemyActionCount> kSequenceNames{
        "READY",   "START",    "TAUNT",       "WALK",        "RUN",         "FLY",
        "HOVER",   "LANDING",  "WALKTOREADY", "READYTOWALK", "WALKTOREADY", "READYTOWALK",
        "ATTACK1", "ATTACK1R", "ATTACK2",     "ATTACK2R",    "ATTACK3",     "ATTACK3R",
        "ATTACK4", "ATTACK4R", "ATTACK5",     "ATTACK5R",    "RUNATTACK1",  "RUNATTACK2",
        "THROW1",  "THROW2",   "THROWF",      "ATTTOREADY",  "HIT1",        "HIT2",
        "HIT3",    "GETUP",    "DEATH"};

    /** How loudly each action asks: a request is refused by a pending one at least as loud. */
    static constexpr std::array<int, kEnemyActionCount> kPriorities{
        100, 900, 900, 200, 200, 200, 200, 200, 200, 200, 200, 200, 300, 300, 300, 300, 300,
        300, 300, 300, 300, 300, 300, 300, 300, 300, 300, 150, 400, 450, 460, 900, 999};

    /** Takes the kind's tree; false when it has no stance. The entrance plays first, and
     * `walksIn` kinds go straight from it to walking. */
    bool bind(const TreeInfo& tree, bool walksIn = false);
    void unbind();
    bool bound() const { return m_tree != nullptr; }

    /** Asks for an action this tick. Attacks and throws are refused while the body idles
     * after a throw. */
    void request(Action action);
    /** Steps `ticks` of the game clock (`seconds` long), answering the tick's requests, which
     * are then forgotten. `contact` says a player is against the body, which some attacks
     * chain on. */
    void update(int ticks, float seconds, bool contact = false);

    Action action() const { return m_current; }
    Action requested() const { return m_requested; }
    bool attacking() const {
        return m_current >= Action::Attack && m_current <= Action::ThrowToReady;
    }
    bool swinging() const { return m_current >= Action::Attack && m_current <= Action::RunAttack2; }
    bool reacting() const { return m_current >= Action::HitReact1; }
    bool dying() const { return m_current == Action::Dying; }
    /** Whether the death has played out. */
    bool dead() const { return m_dead; }
    bool entering() const { return m_current == Action::Start; }
    bool moving() const {
        return m_current == Action::Walk || m_current == Action::Run || m_current == Action::Fly;
    }
    /** Whether this tick's step landed a blow, a stronger one, or let a missile go. */
    bool struck() const { return m_struck; }
    bool powerStruck() const { return m_powerStruck; }
    bool threw() const { return m_threw; }
    /** Seconds the body idles after a throw before the next attack or throw. */
    void setIdle(float seconds) { m_idleSeconds = seconds; }
    float idleSeconds() const { return m_idleSeconds; }
    bool has(Action action) const { return m_sequences[static_cast<std::size_t>(action)] >= 0; }
    /** The sequence an action plays, falling back to the stance when the tree lacks it. */
    unsigned int sequenceOf(Action action) const;
    const TreePose& pose() const { return m_pose; }
    const AnimationPlayer& player() const { return m_player; }

private:
    /** When a decided action may start: the original's cut-in rules. */
    enum class Cut : std::uint8_t { WhenDoneIfDifferent, IfDifferent };

    struct Decision {
        Action action = Action::Ready;
        Cut cut = Cut::IfDifferent;
        bool repeat = true;
    };

    Decision decide(Action next, bool contact) const;
    void play(Decision decision, float seconds);

    const TreeInfo* m_tree = nullptr;
    std::array<int, kEnemyActionCount> m_sequences{};
    Action m_current = Action::Ready;
    Action m_requested = Action::Ready;
    bool m_walksIn = false;
    bool m_struck = false;
    bool m_powerStruck = false;
    bool m_threw = false;
    bool m_dead = false;
    float m_idleSeconds = 0.0f;
    AnimationPlayer m_player;
    TreePose m_pose;
    TreePose m_previous;
};

} // namespace gdl::game
