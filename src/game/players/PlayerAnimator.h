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
    Flinch,      ///< knocked back by a blow
    Spike,       ///< struck by spikes or a blade
    Reel,        ///< stunned, as by a fire trap
    TurboStrong, ///< the lesser turbo attack
    TurboFull,   ///< the greater
    Shove,
    Defend,       ///< held: the guard comes up and stays up
    StrongAttack, ///< the slow attack: a strong throw, with nothing in reach
    ShieldPotion, ///< a potion spent on a ring of its magic about the character
    FallBack,     ///< knocked off its feet from in front
    FallForward,  ///< or from behind
    // A legend item let fly at a boss, with the gesture its kind asks for: nothing else
    // leaves the hand meanwhile.
    HurlLegend,  ///< as a potion is used
    ThrowLegend, ///< as the strong throw
    ShootLegend, ///< the special shot
    Grabbed,
    Thrown,
    Webbed,
    Melee,
    MeleeLow,
    MeleeSlow,
    MeleeSlowLow,
    AutoMelee, ///< a moving body's close contact, not an attack-button press
    AutoMeleeLow,
    SuperShot,
    Hammer,
    Breathe,
    FireLeft,
    FireRight,
    Pick,       ///< the gesture of an item picked up
    Gag,        ///< retching at gas, Death's touch or food gone bad
    DeathGrab,  ///< a halo's hold on Death, drawing him off
    Whirled,    ///< swept up by a whirlwind
    Combo,      ///< takes hold of a partner for the class's two-player combo
    ComboHeld,  ///< in a partner's hands, playing the grabber's class's COMBO sequence
    ComboThrown ///< let go of by the partner: the warrior's pinball, the dwarf's charger
};

/** How far the nearest thing to strike lies: within a swing, within a step, or beyond. */
enum class MeleeRange : u8 { Beyond, Step, Swing };

/** What a close attack knows of the nearest thing to strike (AnimAction's collision bits). */
struct MeleeSense {
    MeleeRange range = MeleeRange::Swing;
    bool low = false; ///< short enough, and near enough, for a low strike
    f32 yaw = 0.0f;   ///< the way to it against the facing, from -pi to pi
};

/** What a completed swing does to what it strikes. */
enum class MeleeBlow : u8 {
    None,
    Plain, ///< the character's own harm
    Heavy, ///< twice it, knocking back
    Kick,  ///< its own harm, knocking a short body down
    Power  ///< three times it, knocking down
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
        Throw,       ///< the wind-up from a stand
        ThrowMoving, ///< the wind-up cut in from a first half of walking or running
        ThrowRelease,
        ThrowMovingRelease,
        ThrowRecover,
        ThrowMovingRecover,
        UsePotion, ///< the hand raised
        UsePotionRelease,
        ThrowPotion,
        ThrowPotionRelease,
        Death,    ///< falls and stays down
        HitReact, ///< recoils from a blow
        Stun,     ///< reels, stunned
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
        FallBack, ///< onto its back
        GetUpBack,
        FallForward, ///< onto its face
        GetUpForward,
        SpecialShot, ///< the special shot's wind-up, at whose end the legend item leaves
        SpecialShotRecover,
        SpikeHit,
        Grabbed,
        WebReact,
        Quick1,
        Quick2,
        Quick3,
        Quick2Recover,
        Quick3Recover,
        SlowStart,
        SlowSwing,
        SlowRecover,
        LowKick,
        LowKickRecover,
        Low1,
        Low2,
        LowRecover,
        // The quick swings turned to a side or behind, in the table's own order: the first
        // of each pair follows the first and third swings, the second follows the second.
        Right,
        Right2,
        RightRecover,
        Right2Recover,
        Left,
        Left2,
        LeftRecover,
        Left2Recover,
        Turn,
        Turn2,
        TurnRecover,
        Turn2Recover,
        TurnLeft,
        TurnLeft2,
        TurnLeftRecover,
        TurnLeft2Recover,
        Spin, ///< the third strong press of a chain
        SpinRecover,
        Step1, ///< a swing that steps into what is a pace away
        Step2,
        Step3,
        Step2Recover,
        Step3Recover,
        WalkStrike, ///< the step cut in from a second half of walking or running
        WalkStrikeRecover,
        PowerClose, ///< a strong press after the first swing of a chain
        PowerCloseRecover,
        PowerMed, ///< after the second
        PowerMedRecover,
        PowerLow, ///< the close one at something short
        PowerLowRecover,
        SpecialShotRepeat,
        Hammer,
        HammerRecover,
        Breathe,
        BreatheRecover,
        FireLeft,
        FireLeftRecover,
        FireRight,
        FireRightRecover,
        Pick,             ///< a hand to the ground for what was taken, at full pace
        Gag,              ///< STUN2, looped while it lasts
        ShieldReady,      ///< the stance with a shield on the arm
        ShieldRun,        ///< and its gait, walking or running
        DeathGrabStart,   ///< the hands going out to Death
        DeathGrab,        ///< held on him, looped
        DeathGrabRelease, ///< and let go
        Whirled,          ///< flung up by a whirlwind (P_WHIRLWIND), then up again
        Pushed,           ///< shoved by another member, in place of standing or walking
        // The two-player combo: the grabber's move, then what its partner plays, in the
        // original's order (COMBOACT1..3, COMBOWAR1..COMBOJES).
        ComboAct1,
        ComboAct2, ///< the dwarf's ride, looped while it lasts
        ComboAct3,
        ComboWar1, ///< in the warrior's hands
        ComboWar2, ///< the pinball, looped while it flies
        ComboWar3, ///< and its landing
        ComboVal,  ///< lifting the valkyrie
        ComboWiz,  ///< in the wizard's hands
        ComboArc,  ///< lifting the archer
        ComboDwf1, ///< the dwarf climbing on
        ComboDwf2, ///< the charger, looped while it is steered
        ComboDwf3, ///< and its end
        ComboKni,  ///< in the knight's hands
        ComboSor,  ///< in the sorceress's
        ComboJes   ///< in the jester's
    };
    /** The foot that came down as a walk or run half cycle ended. */
    enum class Foot : u8 { None, First, Second };
    static constexpr usize kActionCount = 132;
    static constexpr std::array<std::string_view, kActionCount> kSequenceNames{
        "READY",        "IDLE1",        "IDLE2",        "IDLE2_LOOP",   "WALK1",
        "WALK2",        "RUN1",         "RUN2",         "START",        "THROW1S",
        "THROW2S",      "THROW1",       "THROW2",       "THROW1R",      "THROW2R",
        "MAGICS",       "MAGICR",       "THROWPOTIONS", "THROWPOTIONR", "DEATH",
        "HITREACT",     "STUN1",        "ATTPWRB",      "ATTPWRC",      "SHOVE",
        "DEFEND1",      "DEFEND2",      "DEFENDR",      "ATTPWRATHROW", "ATTPWRATHROWR",
        "STRAFE_WLKF1", "STRAFE_WLKF2", "STRAFE_WLKB1", "STRAFE_WLKB2", "STRAFE_WLKL1",
        "STRAFE_WLKL2", "STRAFE_WLKR1", "STRAFE_WLKR2", "STRAFE_ATKF1", "STRAFE_ATKF2",
        "STRAFE_ATKB1", "STRAFE_ATKB2", "STRAFE_ATKL1", "STRAFE_ATKL2", "STRAFE_ATKR1",
        "STRAFE_ATKR2", "FALLDOWN",     "GETUP",        "FALLFRNT",     "GETUP2",
        "SSHOT1",       "SSHOTR",       "SPIKEHIT",     "GRABBED",      "WEBREACT",
        "ATTQUICK1",    "ATTQUICK2",    "ATTQUICK3",    "ATTQUICK2R",   "ATTQUICK3R",
        "ATTSTART",     "ATTSLOW1",     "ATTSLOW1R",    "ATTLOWK",      "ATTLOWKR",
        "ATTLOW1",      "ATTLOW2",      "ATTLOWR",      "ATTQ3RIGHT",   "ATTQ2RIGHT",
        "ATTQ3RIGHTR",  "ATTQ2RIGHTR",  "ATTQ3LEFT",    "ATTQ2LEFT",    "ATTQ3LEFTR",
        "ATTQ2LEFTR",   "ATTQ2180",     "ATTQ3180",     "ATTQ2180R",    "ATTQ3180R",
        "ATTQ2180L",    "ATTQ3180L",    "ATTQ2180LR",   "ATTQ3180LR",   "ATT360",
        "ATT360R",      "ATTSTEP1",     "ATTSTEP2",     "ATTSTEP3",     "ATTSTEP2R",
        "ATTSTEP3R",    "ATTWALK2",     "ATTWALK2R",    "ATTPWRACLOSE", "ATTPWRACLOSER",
        "ATTPWRAMED",   "ATTPWRAMEDR",  "ATTPWRALOW",   "ATTPWRALOWR",  "SSHOT2",
        "ATTCHOP",      "ATTCHOPR",     "ATTBREATHE",   "ATTBREATHER",  "ATTFIREL",
        "ATTFIRELR",    "ATTFIRER",     "ATTFIRERR",    "PICK",         "STUN2",
        "SHIELD_READY", "SHIELD_RUN",   "DEATHGRABS",   "DEATHGRAB",    "DEATHGRABR",
        "FLYUP",        "PUSHED",       "COMBOACT1",    "COMBOACT2",    "COMBOACT3",
        "COMBOWAR1",    "COMBOWAR2",    "COMBOWAR3",    "COMBOVAL",     "COMBOWIZ",
        "COMBOARC",     "COMBODWF1",    "COMBODWF2",    "COMBODWF3",    "COMBOKNI",
        "COMBOSOR",     "COMBOJES"};
    static constexpr f32 kReleaseFrame = 2.0f;     ///< of the wind-up, from which it gives way
    static constexpr f32 kGagHold = 10.0f;         ///< frames of retching before anything cuts in
    static constexpr s32 kFidgetTicks = 1800;      ///< standing still before the first fidget
    static constexpr s32 kSecondFidgetTicks = 600; ///< after the first before the second
    static constexpr f32 kRunMagnitude = 0.75f;    ///< stick beyond this runs
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
    /** What the close attack knows of its nearest target, set before each update. */
    void setMelee(const MeleeSense& sense) { m_melee = sense; }
    /** The class of the partner making the combo this body is held or thrown in (which
     * COMBO sequence it plays), and whether the dwarf's ride is to go on (COMBOACT2 loops
     * while it is). Set before each update. */
    void setCombo(s32 grabberClass, bool ride) {
        m_comboClass = grabberClass;
        m_comboRide = ride;
    }
    /** Whether the body is making its class's combo move (COMBOACT1..3). */
    bool comboing() const {
        return m_current >= Action::ComboAct1 && m_current <= Action::ComboAct3;
    }
    /** Whether the body is in a partner's combo: held, flying or landing. */
    bool comboBound() const {
        return m_current >= Action::ComboWar1 && m_current <= Action::ComboJes;
    }
    /** Whether the body is the warrior's pinball or the dwarf's charger. */
    bool comboThrown() const {
        return m_current == Action::ComboWar2 || m_current == Action::ComboDwf2;
    }
    /** Whether a partner may take hold of the body now (fn_80088EF4's actions 84 to 90 and
     * from 107): not in a power swing, a turbo move, a potion, a guard, a reaction, its
     * entrance, its death or a combo. */
    bool comboTakeable() const {
        return !entering() && !dying() && !reacting() && !turboing() && !conjuring() &&
               !guarding() && !comboBound() && m_current != Action::PowerLow &&
               m_current != Action::PowerLowRecover;
    }
    /** What a partner held by a grabber of `grabberClass` plays (PlayerMotion's case 38). */
    static Action comboHeldActionOf(s32 grabberClass);
    /** What a partner let go of by one plays (case 39): only the warrior's and the dwarf's. */
    static Action comboThrownActionOf(s32 grabberClass);
    /** Whether the body is holding Death, or reaching for him or letting him go. */
    bool grabbingDeath() const {
        return m_current >= Action::DeathGrabStart && m_current <= Action::DeathGrabRelease;
    }
    /** Whether a shield is borne on the arm: the stance and gait are the shield's. */
    void setShielded(bool shielded) { m_shielded = shielded; }
    /** Whether another member is pushing the body along: standing, walking or running, it
     * shows being pushed instead (P_PUSHED, pmotion.c 2042). Set before each update. */
    void setPushed(bool pushed) { m_pushed = pushed; }
    /** The character whose body this is: some classes keep their feet in the power swings. */
    void setCharacter(s32 character);
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
        return (m_current >= Action::FallBack && m_current <= Action::GetUpForward) ||
               m_current == Action::Whirled;
    }
    /** Whether this tick's step began a shield potion's release. */
    bool potionShielded() const { return m_potionShielded; }

    static PlayerMotion motionFor(f32 stickMagnitude);

    Action action() const { return m_current; }
    /** Whether this action belongs to the attack groups that provoke a creature's block. */
    static bool isBlockableAttack(Action action);
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
    /** Held game ticks during the potion's wind-up; frozen on first release. */
    s32 potionThrowTicks() const { return m_potionThrowTicks; }
    /** Whether the weapon has left the hand and the body is recovering from the throw. */
    bool recovering() const {
        return m_current == Action::ThrowRecover || m_current == Action::ThrowMovingRecover;
    }
    /** Whether this tick's step ended a release: the moment the weapon flies. */
    bool released() const { return m_released; }
    bool meleeing() const { return isMelee(m_current); }
    /** One contact at the completed swing, never a projectile release. */
    bool meleeStruck() const { return m_meleeBlow != MeleeBlow::None; }
    /** What this tick's completed swing does to what it strikes. */
    MeleeBlow meleeBlow() const { return m_meleeBlow; }
    /** Strikes counted into the current chain of close attacks (none outside one). */
    s32 meleeChain() const { return m_chain; }
    /** Whether unpressed walking/running may begin a contact swing. Buffered attack
     * edges and actions already owning the body must finish first. */
    bool canAutoMelee() const;
    static bool isMelee(Action action) {
        return action >= Action::Quick1 && action <= Action::PowerLowRecover;
    }
    /** How long the attack had been going when the weapon was let go. */
    f32 attackSeconds() const { return m_attackSeconds; }
    /** The arrival is pending or still playing; player input must wait. */
    bool entering() const { return !m_entered || m_current == Action::Start; }
    /** How much of its pace the current action leaves the body. */
    f32 moveScale() const {
        if (entering()) {
            return 0.0f;
        }
        if (webbed()) {
            return kWebPace;
        }
        if (grabbingDeath() || comboBound()) {
            return 0.0f; // a body in a partner's combo goes where the combo takes it
        }
        if (shoving()) {
            return kChargePace; // the charge rushes on, faster than a run
        }
        if (meleeing()) {
            return meleePace();
        }
        if (strongThrowing()) {
            return kStrongThrowPace;
        }
        if (running() || m_current == Action::ShieldRun) {
            return kRunPace;
        }
        if (strafing()) {
            return kStrafePace;
        }
        return throwing() || meleeing() || conjuring() || reacting() || turboing() || guarding()
                   ? 0.0f
                   : 1.0f;
    }
    static constexpr f32 kChargePace = 1.5f;
    static constexpr f32 kRunPace = 1.3f;      ///< a run covers more ground than a walk
    static constexpr f32 kStrafePace = 0.667f; ///< strafing steps, shooting or not
    static constexpr f32 kWebPace = 0.4f;
    static constexpr f32 kQuickMeleePace = 0.25f;
    static constexpr f32 kStrongThrowPace = 0.25f;
    bool quickMeleeing() const {
        return m_current >= Action::Quick1 && m_current <= Action::Quick3Recover;
    }
    /** How far the current action lets the body turn toward the stick: none, part of the way
     * each 30 Hz frame, or all of it. */
    f32 turnScale() const;
    bool running() const { return m_current == Action::Run1 || m_current == Action::Run2; }
    /** Whether the body is in a stepping swing or recovering from one: it carries on at half
     * its pace with the stick let go. */
    bool lunging() const {
        return m_current >= Action::Step1 && m_current <= Action::WalkStrikeRecover;
    }
    /** Web contact suppresses attacks, but leaves a slow escape walk. */
    bool webbed() const { return m_current == Action::WebReact; }
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
               m_current == Action::Shove || strongThrowing() || specialShooting() ||
               itemAttacking() || comboing();
    }
    /** Whether the body is in the special shot or recovering from it. */
    bool specialShooting() const {
        return m_current == Action::SpecialShot || m_current == Action::SpecialShotRecover ||
               m_current == Action::SpecialShotRepeat;
    }
    /** Attack-state invulnerability, including combo release/landing. Ordinary strong
     * throws, power swings and shoves remain vulnerable despite locking their animation. */
    bool damageProtected() const {
        return m_current == Action::TurboStrong || m_current == Action::TurboFull ||
               specialShooting() || m_current == Action::Hammer ||
               m_current == Action::HammerRecover || m_current == Action::Breathe ||
               m_current == Action::BreatheRecover || comboing() || comboBound();
    }
    /** Whether the body is making a legend item's gesture: the moment its wind-up ends is
     * `legendReleased`, and no potion or weapon goes with it. */
    bool castingLegend() const { return m_legendAsked; }
    bool legendReleased() const { return m_legendReleased; }
    bool superReleased() const { return m_superReleased; }
    bool itemAttacking() const {
        return m_current >= Action::Hammer && m_current <= Action::FireRightRecover;
    }
    PlayerDeed itemReleased() const { return m_itemReleased; }
    /** Item animation multipliers are latched when the next sequence starts;
     * movement, simulation and transition clocks remain unchanged. */
    void setAttackSpeed(bool rapid, bool speed) {
        m_rapid = rapid;
        m_speed = speed;
    }
    /** Whether the body is in the strong throw or recovering from it. */
    bool strongThrowing() const {
        return m_current == Action::StrongThrow || m_current == Action::StrongThrowRecover;
    }
    /** Whether this tick's step ended the strong throw's wind-up: the weapon flies. */
    bool strongReleased() const { return m_strongReleased; }
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
    /** Whether a hit reaction owns the animation. Webs still permit slow movement. */
    bool reacting() const {
        return m_current == Action::HitReact || m_current == Action::Stun ||
               m_current == Action::SpikeHit || m_current == Action::Grabbed ||
               m_current == Action::WebReact || floored();
    }
    /** The footfall this tick, if a half cycle of walking or running just ended. */
    Foot footfall() const { return m_footfall; }
    const TreePose& pose() const { return m_pose; }
    /** Visual-only fractional pose, interpolated between simulation updates. Gameplay
     * attachments and attack windows must continue to use pose() and frame(). */
    void evaluatePresentation(TreePose& out, f32 alpha = 1.0f) const;
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
        explicit Decision(Action request = Action::Ready) : action(request), requested(request) {}
        Action action;
        Action requested; ///< before follow-up/refinement, for native item-speed eligibility
        Cut cut = Cut::WhenDoneIfDifferent;
        bool repeat = false;
        f32 transition = 0.0f;
        f32 startFrame = 0.0f;
    };

    static bool isThrow(Action action) {
        return action >= Action::Throw && action <= Action::ThrowMovingRecover;
    }
    Decision decide(Action requested) const;
    f32 meleePace() const;
    Action meleeRequest(PlayerDeed deed, bool moved) const;
    Action chainAfter(Action swing) const;
    Action refine(Action action) const;
    static Action powerOf(s32 chain);
    bool playable(Action action) const;
    /** A strong press buffered within a chain: the next swing is a power one. */
    bool buffered() const { return m_strongPress && m_chain != 0; }
    static Action recoveryOf(Action swing);
    static MeleeBlow blowOf(Action swing);
    static bool retainsAttackPress(Action action);
    f32 animationDuration(const Decision& decision) const;
    void play(const Decision& decision, f32 seconds);

    const TreeInfo* m_tree = nullptr;
    std::array<s32, kActionCount> m_sequences{};
    Action m_current = Action::Ready;
    Foot m_footfall = Foot::None;
    bool m_entered = true; ///< the entrance has played (or was not asked for)
    bool m_released = false;
    MeleeBlow m_meleeBlow = MeleeBlow::None;
    MeleeSense m_melee;
    s32 m_character = -1;
    s32 m_chain = 0;            ///< strikes in the current chain, each on a fresh press
    bool m_quickPress = false;  ///< a fresh press of the attack since the last strike began
    bool m_strongPress = false; ///< of the strong attack
    bool m_quickHeld = false;   ///< last update's buttons, to tell a press
    bool m_strongHeld = false;
    bool m_moved = false; ///< the stick asked for movement this update
    bool m_shielded = false;
    bool m_pushed = false;
    bool m_potionUsed = false;
    bool m_potionThrown = false;
    s32 m_potionThrowTicks = 0;
    bool m_potionThrowReleased = false;
    bool m_dead = false;
    bool m_turboBegan = false;
    bool m_strongReleased = false;
    bool m_potionShielded = false;
    bool m_shieldAsked = false; ///< the potion being used is for a shield
    bool m_legendAsked = false; ///< the gesture under way is a legend item's
    bool m_legendReleased = false;
    bool m_superReleased = false;
    bool m_superHeld = false;
    PlayerDeed m_itemReleased = PlayerDeed::None;
    bool m_rapid = false;
    bool m_speed = false;
    StrafeWay m_strafe = StrafeWay::None;
    s32 m_comboClass = -1;      ///< the grabber's class, for a body held or thrown in a combo
    bool m_comboRide = false;   ///< the dwarf's ride goes on: COMBOACT2 loops
    bool m_potionLatch = false; ///< a potion has gone for this press of its button
    f32 m_attackSeconds = 0.0f; ///< since the attack began, while it goes on
    s32 m_stillTicks = 0;       ///< ticks standing still
    s32 m_fidgetTicks = 0;      ///< ticks since the first fidget, 0 before it
    AnimationPlayer m_player;
    TreePose m_pose;
    TreePose m_previous; ///< what showed when the current sequence started, for blending
    TreePose m_presentationPrevious;
    u64 m_presentationGeneration = 0;
};

} // namespace gdl::game
