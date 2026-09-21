#pragma once

#include <array>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreePose.h"

namespace gdl::game {

/** What a player's stick asks of the body. */
enum class PlayerMotion : u8 { Stand, Walk, Run };

/** What a player's buttons ask of it, the first that is held winning. */
enum class PlayerDeed : u8 {
    None,
    Attack,
    UsePotion,
    ThrowPotion,
    Die,
    Flinch, ///< struck by spikes or a blade
    Reel,   ///< stunned, as by a fire trap
    TurboStrong, ///< the lesser turbo attack
    TurboFull,   ///< the greater
    Shove
};

/**
 * The actions a character's body plays, sequenced the way the original game does: the
 * entrance once as the level begins, the stance loop, a fidget after a minute standing still
 * and a second one twenty seconds later that then loops, the two halves of the walk and run
 * cycles taking turns, and the throw of its weapon while the attack is held (a wind-up cut
 * short at its second frame, the release, whose end lets the weapon go, and the recovery,
 * after which the next throw starts or the body eases back to its stance), and a potion
 * used where it stands or thrown (a raising of the hand, then a release whose start is the
 * moment the magic goes off or the bottle flies). Each tick the
 * request becomes a decision (which action, when it may cut in, whether it loops, how long
 * it blends), the sequence steps, and the pose is evaluated for drawing.
 */
class PlayerAnimator {
public:
    enum class Action : u8 {
        Ready,
        Idle1,
        Idle2,
        Idle2Loop,
        Walk1,
        Walk2,
        Run1,
        Run2,
        Start,
        Throw,              ///< the wind-up from a stand
        ThrowMoving,        ///< the wind-up cut in from a first half of walking or running
        ThrowRelease,
        ThrowMovingRelease,
        ThrowRecover,
        ThrowMovingRecover,
        UsePotion,          ///< the hand raised
        UsePotionRelease,
        ThrowPotion,
        ThrowPotionRelease,
        Death,              ///< falls and stays down
        HitReact,           ///< flinches from spikes or a blade
        Stun,               ///< reels, stunned
        TurboStrong,
        TurboFull,
        Shove
    };
    /** The foot that came down as a walk or run half cycle ended. */
    enum class Foot : u8 { None, First, Second };
    static constexpr usize kActionCount = 25;
    static constexpr std::array<std::string_view, kActionCount> kSequenceNames{
        "READY",  "IDLE1",  "IDLE2",        "IDLE2_LOOP",  "WALK1",  "WALK2",   "RUN1",
        "RUN2",   "START",  "THROW1S",      "THROW2S",     "THROW1", "THROW2",  "THROW1R",
        "THROW2R", "MAGICS", "MAGICR",      "THROWPOTIONS", "THROWPOTIONR", "DEATH",
        "HITREACT", "STUN1", "ATTPWRB", "ATTPWRC", "SHOVE"};
    static constexpr f32 kReleaseFrame = 2.0f; ///< of the wind-up, from which it gives way
    static constexpr s32 kFidgetTicks = 1800;         ///< standing still before the first fidget
    static constexpr s32 kSecondFidgetTicks = 600;    ///< after the first before the second
    static constexpr f32 kRunMagnitude = 0.75f;       ///< stick beyond this runs
    static constexpr f32 kStanceBlend = 2.0f / 30.0f; ///< seconds a body eases back into its stance

    /** Takes the class tree's sequences; false when it has no stance. With `enter` the
     * entrance sequence plays before anything else. */
    bool bind(const TreeInfo& tree, bool enter = true);
    void unbind();
    bool bound() const { return m_tree != nullptr; }

    /** Steps `ticks` of the game clock (`seconds` long) under `motion`, throwing while
     * `attack` is held. */
    void update(PlayerMotion motion, s32 ticks, f32 seconds, bool attack = false) {
        update(motion, ticks, seconds, attack ? PlayerDeed::Attack : PlayerDeed::None);
    }
    void update(PlayerMotion motion, s32 ticks, f32 seconds, PlayerDeed deed);

    static PlayerMotion motionFor(f32 stickMagnitude);

    Action action() const { return m_current; }
    /** Whether the body is anywhere in a throw; its feet stay where they are meanwhile. */
    bool throwing() const { return isThrow(m_current); }
    /** Whether the body is busy with a potion. */
    bool conjuring() const {
        return m_current >= Action::UsePotion && m_current <= Action::ThrowPotionRelease;
    }
    /** Whether the body is falling dead or lies dead; nothing else is asked of it then. */
    bool dying() const { return m_current == Action::Death; }
    /** Whether it has finished falling (at once for a class with no such sequence). */
    bool dead() const { return m_dead; }
    /** Whether this tick's step began a potion's release: its magic goes off, or it flies. */
    bool potionUsed() const { return m_potionUsed; }
    bool potionThrown() const { return m_potionThrown; }
    /** Whether the weapon has left the hand and the body is recovering from the throw. */
    bool recovering() const {
        return m_current == Action::ThrowRecover || m_current == Action::ThrowMovingRecover;
    }
    /** Whether this tick's step ended a release: the moment the weapon flies. */
    bool released() const { return m_released; }
    /** How long the attack had been going when the weapon was let go. */
    f32 attackSeconds() const { return m_attackSeconds; }
    /** How much of its pace the current action leaves the body. */
    f32 moveScale() const {
        return throwing() || conjuring() || reacting() || turboing() ? 0.0f : 1.0f;
    }
    /** Whether the body is in a turbo move, which plays through with nothing else heeded. */
    bool turboing() const {
        return m_current == Action::TurboStrong || m_current == Action::TurboFull ||
               m_current == Action::Shove;
    }
    /** Whether the body can begin the turbo move `deed` now: it has the sequence and is not
     * in the middle of anything. */
    bool canBegin(PlayerDeed deed) const;
    /** The action a turbo deed plays; the stance for any other deed. */
    static Action turboActionOf(PlayerDeed deed);
    /** Whether this tick's step began a turbo move: the meter pays for it then. */
    bool turboBegan() const { return m_turboBegan; }
    /** Whether the body is flinching or reeling from a hit: it stands where it was struck,
     * does nothing else, and is not set reeling again until it is over. */
    bool reacting() const { return m_current == Action::HitReact || m_current == Action::Stun; }
    /** The footfall this tick, if a half cycle of walking or running just ended. */
    Foot footfall() const { return m_footfall; }
    const TreePose& pose() const { return m_pose; }
    const AnimationPlayer& player() const { return m_player; }
    s32 stillTicks() const { return m_stillTicks; }
    s32 fidgetTicks() const { return m_fidgetTicks; }
    /** The sequence an action plays, falling back to the stance when the tree lacks it. */
    u32 sequenceOf(Action action) const;

private:
    /** When a decided action may start: the original's four cut-in rules. */
    enum class Cut : u8 { WhenDoneIfDifferent, WhenDone, IfDifferent, Now };

    /** How the current action answers a request. */
    struct Decision {
        Action action = Action::Ready;
        Cut cut = Cut::WhenDoneIfDifferent;
        bool repeat = false;
        f32 transition = 0.0f;
    };

    static bool isThrow(Action action) {
        return action >= Action::Throw && action <= Action::ThrowMovingRecover;
    }
    Decision decide(Action requested) const;
    void play(const Decision& decision, f32 seconds);

    const TreeInfo* m_tree = nullptr;
    std::array<s32, kActionCount> m_sequences{};
    Action m_current = Action::Ready;
    Foot m_footfall = Foot::None;
    bool m_entered = true; ///< the entrance has played (or was not asked for)
    bool m_released = false;
    bool m_potionUsed = false;
    bool m_potionThrown = false;
    bool m_dead = false;
    bool m_turboBegan = false;
    bool m_potionLatch = false; ///< a potion has gone for this press of its button
    f32 m_attackSeconds = 0.0f; ///< since the attack began, while it goes on
    s32 m_stillTicks = 0;  ///< ticks standing still
    s32 m_fidgetTicks = 0; ///< ticks since the first fidget, 0 before it
    AnimationPlayer m_player;
    TreePose m_pose;
    TreePose m_previous; ///< what showed when the current sequence started, for blending
};

} // namespace gdl::game
