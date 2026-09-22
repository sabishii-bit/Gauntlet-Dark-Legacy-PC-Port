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
    Shove,
    Defend,      ///< held: the guard comes up and stays up
    StrongAttack, ///< the slow attack: a strong throw, with nothing in reach
    ShieldPotion, ///< a potion spent on a ring of its magic about the character
    FallBack,     ///< knocked off its feet from in front
    FallForward,  ///< or from behind
    // A legend item let fly at a boss, with the gesture its kind asks for: nothing else
    // leaves the hand meanwhile.
    HurlLegend,   ///< as a potion is used
    ThrowLegend,  ///< as the strong throw
    ShootLegend   ///< the special shot
};

/** Which way a strafing character steps, against the way it faces. */
enum class StrafeWay : u8 { None, Forward, Back, Left, Right };

/**
 * The actions a character's body plays, sequenced the way the original game does: the
 * entrance once as the level begins, the stance loop, a fidget after a minute standing still
 * and a second one twenty seconds later that then loops, the two halves of the walk and run
 * cycles taking turns, and the throw of its weapon while the attack is held (a wind-up cut
 * short at its second frame, the release, whose end lets the weapon go, and the recovery,
 * after which the next throw starts or the body eases back to its stance), and a potion
 * used where it stands or thrown (a raising of the hand, then a release whose start is the
 * moment the magic goes off or the bottle flies), and a legend item let fly with one of
 * those gestures or the special shot, nothing else leaving the hand. Each tick the
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
        Shove,
        DefendRaise, ///< the guard coming up
        Defend,      ///< held up, which is when it blocks
        DefendLower,
        StrongThrow, ///< the strong throw's wind-up, at whose end the weapon leaves
        StrongThrowRecover,
        // Strafing, two half cycles a way, in the order of StrafeWay; then the same attacking.
        StrafeForward1,
        StrafeForward2,
        StrafeBack1,
        StrafeBack2,
        StrafeLeft1,
        StrafeLeft2,
        StrafeRight1,
        StrafeRight2,
        StrafeShootForward1,
        StrafeShootForward2,
        StrafeShootBack1,
        StrafeShootBack2,
        StrafeShootLeft1,
        StrafeShootLeft2,
        StrafeShootRight1,
        StrafeShootRight2,
        FallBack,    ///< onto its back
        GetUpBack,
        FallForward, ///< onto its face
        GetUpForward,
        SpecialShot, ///< the special shot's wind-up, at whose end the legend item leaves
        SpecialShotRecover
    };
    /** The foot that came down as a walk or run half cycle ended. */
    enum class Foot : u8 { None, First, Second };
    static constexpr usize kActionCount = 52;
    static constexpr std::array<std::string_view, kActionCount> kSequenceNames{
        "READY",  "IDLE1",  "IDLE2",        "IDLE2_LOOP",  "WALK1",  "WALK2",   "RUN1",
        "RUN2",   "START",  "THROW1S",      "THROW2S",     "THROW1", "THROW2",  "THROW1R",
        "THROW2R", "MAGICS", "MAGICR",      "THROWPOTIONS", "THROWPOTIONR", "DEATH",
        "HITREACT", "STUN1", "ATTPWRB", "ATTPWRC", "SHOVE", "DEFEND1", "DEFEND2", "DEFENDR",
        "ATTPWRATHROW", "ATTPWRATHROWR",
        "STRAFE_WLKF1", "STRAFE_WLKF2", "STRAFE_WLKB1", "STRAFE_WLKB2", "STRAFE_WLKL1",
        "STRAFE_WLKL2", "STRAFE_WLKR1", "STRAFE_WLKR2", "STRAFE_ATKF1", "STRAFE_ATKF2",
        "STRAFE_ATKB1", "STRAFE_ATKB2", "STRAFE_ATKL1", "STRAFE_ATKL2", "STRAFE_ATKR1",
        "STRAFE_ATKR2", "FALLDOWN", "GETUP", "FALLFRNT", "GETUP2", "SSHOT1", "SSHOTR"};
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
    /** Which way the character strafes from now on (none: it walks and runs as ever). Set
     * before each update: moving, it steps that way with its facing held, and an attack asked
     * of it is made as it goes. */
    void setStrafe(StrafeWay way) { m_strafe = way; }
    /** Whether the body is in a strafing step, shooting or not. */
    bool strafing() const {
        return m_current >= Action::StrafeForward1 && m_current <= Action::StrafeShootRight2;
    }
    /** Whether the body is off its feet or getting back onto them. */
    bool floored() const {
        return m_current >= Action::FallBack && m_current <= Action::GetUpForward;
    }
    /** Whether this tick's step began a shield potion's release. */
    bool potionShielded() const { return m_potionShielded; }

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
        if (shoving()) {
            return kChargePace; // the charge rushes on, faster than a run
        }
        if (strongThrowing()) {
            return kStrongThrowPace;
        }
        return throwing() || conjuring() || reacting() || turboing() || guarding() ? 0.0f : 1.0f;
    }
    static constexpr f32 kChargePace = 1.5f;
    /** Whether the guard is coming up, up or going down; the feet stay put throughout. */
    bool guarding() const {
        return m_current == Action::DefendRaise || m_current == Action::Defend ||
               m_current == Action::DefendLower;
    }
    /** Whether the guard is up, which is when it takes the force out of a blow. */
    bool defending() const { return m_current == Action::Defend; }
    /** Whether the body is shoving, which takes half the force out of a blow. */
    bool shoving() const { return m_current == Action::Shove; }
    /** Whether the body is in a turbo move, which plays through with nothing else heeded. */
    bool turboing() const {
        return m_current == Action::TurboStrong || m_current == Action::TurboFull ||
               m_current == Action::Shove || strongThrowing() || specialShooting();
    }
    /** Whether the body is in the special shot or recovering from it. */
    bool specialShooting() const {
        return m_current == Action::SpecialShot || m_current == Action::SpecialShotRecover;
    }
    /** Whether the body is making a legend item's gesture: the moment its wind-up ends is
     * `legendReleased`, and no potion or weapon goes with it. */
    bool castingLegend() const { return m_legendAsked; }
    bool legendReleased() const { return m_legendReleased; }
    /** Whether the body is in the strong throw or recovering from it. */
    bool strongThrowing() const {
        return m_current == Action::StrongThrow || m_current == Action::StrongThrowRecover;
    }
    /** Whether this tick's step ended the strong throw's wind-up: the weapon flies. */
    bool strongReleased() const { return m_strongReleased; }
    /** How much of its pace the body keeps in the strong throw. */
    static constexpr f32 kStrongThrowPace = 0.25f;
    /** Whether the body can begin the turbo move `deed` now: it has the sequence and is not
     * in the middle of anything. */
    bool canBegin(PlayerDeed deed) const;
    /** The action a turbo deed plays; the stance for any other deed. */
    static Action turboActionOf(PlayerDeed deed);
    /** The first half of the strafing step `way`, shooting or not. */
    static Action strafeStep(StrafeWay way, bool shooting);
    static Action firstHalfOf(Action step);
    static Action otherHalfOf(Action step);
    /** Whether this tick's step began a turbo move: the meter pays for it then. */
    bool turboBegan() const { return m_turboBegan; }
    /** Whether the body is flinching or reeling from a hit: it stands where it was struck,
     * does nothing else, and is not set reeling again until it is over. */
    bool reacting() const {
        return m_current == Action::HitReact || m_current == Action::Stun || floored();
    }
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
    bool m_strongReleased = false;
    bool m_potionShielded = false;
    bool m_shieldAsked = false; ///< the potion being used is for a shield
    bool m_legendAsked = false; ///< the gesture under way is a legend item's
    bool m_legendReleased = false;
    StrafeWay m_strafe = StrafeWay::None;
    bool m_potionLatch = false; ///< a potion has gone for this press of its button
    f32 m_attackSeconds = 0.0f; ///< since the attack began, while it goes on
    s32 m_stillTicks = 0;  ///< ticks standing still
    s32 m_fidgetTicks = 0; ///< ticks since the first fidget, 0 before it
    AnimationPlayer m_player;
    TreePose m_pose;
    TreePose m_previous; ///< what showed when the current sequence started, for blending
};

} // namespace gdl::game
