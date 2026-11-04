#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/ParticleSystem.h"

#include "game/audio/LegendSound.h"
#include "game/players/PlayerDeeds.h"

namespace gdl::game {

/**
 * What a legend item does to the boss it is for, thrown at it as the fight opens: a share
 * of its health (or a flat sum) at once, and a weakness for a while or for good. The items
 * are named by the realm of their boss: the castle's chimera loses a head to the scimitar,
 * the mountain's dragon is frozen by the ice axe, the desert's genie is blinded by the lamp,
 * the forest's spider is poisoned by the bellows, the temple's soul is shaken by the
 * savior, the town's lich is burned by the book, the ice's yeti is melted by the parchment,
 * the dream's wraith is shown by the lantern, and the sky's plague fiend is blinded by the
 * javelin. The underworld's and the battlefield's bosses have none.
 */
struct LegendWeakness {
    s32 boss = -1;              ///< the boss kind, 34 the dragon to 44 the garm
    s32 realm = 0;              ///< the item, by the realm whose boss this is
    f32 healthShare = 0.0f;     ///< of its health now, taken at once
    f32 damage = 0.0f;          ///< or this much, taken at once
    s32 frozenTicks = 0;        ///< it stands frozen this long
    s32 blindTicks = 0;         ///< new moves lose aiming and turning slows to a tenth
    f32 curbSeconds = 0.0f;     ///< over nought its curbed attacks are refused, bursts cut to this
    f32 recoverySeconds = 0.0f; ///< world-update recovery timer after the roar, zero if none
    f32 scale = 1.0f;           ///< how big it stands afterwards
    bool beheads = false;       ///< the scimitar targets the lion head, not a share of body health
    bool operator==(const LegendWeakness&) const = default;

    bool harms() const { return healthShare > 0.0f || damage > 0.0f; }
    bool curbs() const { return curbSeconds > 0.0f; }
    friend void visitNativeState(NativeStateDigest& digest, const LegendWeakness& value) {
        digest.fields<10>(value);
    }
};

/** The weakness of the boss `kind`, or null for one no item touches. */
const LegendWeakness* legendWeaknessOf(s32 kind);
/** The realm of the legend item that weakens the boss `kind`, or nought. */
s32 legendRealmOf(s32 kind);

/** Something the rite did that the game shows. */
enum class LegendCue : u8 {
    Brandished, ///< the bearer holds the item up: it is theirs no more
    Thrown,     ///< the bearer begins the throwing gesture
    Roared,     ///< the boss roars at it
    WornOff     ///< its weakness has passed
};

/**
 * The rite of a legend item at a boss fight's opening, as the original stages it: the
 * bearer waits while the boss rises, then brandishes the item and, a second later, throws
 * it; the boss holds its stance meanwhile, roars (a second after rising for the chimera,
 * the lich and the temple's boss, three for the rest), and fights on weakened until, for
 * some, the weakness wears off.
 */
class LegendRite {
public:
    enum class Stage : u8 { None, Carried, Woken, Struck, Over };

    static constexpr s32 kBrandishTicks = 60;
    static constexpr s32 kShortRoarWait = 60;
    static constexpr s32 kLongRoarWait = 180;

    /** Includes hidden wait/wear clocks and one-shot cue latches. */
    bool equalLocal(const LegendRite& other) const { return *this == other; }

    /** Begins for `player`, who carries the item of `weakness`. */
    void begin(s32 player, const LegendWeakness& weakness);
    void clear();

    /** Moves the rite `ticks` on, told whether the boss has finished rising and whether it
     * has finished roaring; the cues to show come back. */
    std::vector<LegendCue> update(s32 ticks, bool bossRisen, bool bossRoarDone);
    /** The Dragon's projectile hit advances retail state 2/3 to 4 without waiting for a
     * roar; its twenty-second freeze is independent of the presentation's darkness. */
    bool finishOnImpact();

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Over; }
    s32 player() const { return m_player; }
    const LegendWeakness* weakness() const {
        return m_stage != Stage::None ? &m_weakness : nullptr;
    }
    /** Whether the boss holds its stance for the bearer. */
    bool holdsBoss() const { return m_stage == Stage::Carried || m_stage == Stage::Woken; }
    /** Whether the boss should roar now. */
    bool wantsRoar() const { return m_roarDue && !m_roared; }
    bool thrown() const { return m_thrown; }
    /** Whether its weakness is on the boss. */
    bool weakening() const { return m_thrown && m_stage != Stage::Over; }
    /** The opening presentation dims the scene, not the weakness timer. The Dragon's
     * ends on projectile impact; the other bosses' end with their roar. */
    bool darkens() const;
    static constexpr f32 kDarkening = -0.8f; ///< what it takes off the ambient light

private:
    friend void visitNativeState(NativeStateDigest& digest, const LegendRite& value) {
        digest.values(value.m_stage, value.m_weakness, value.m_player, value.m_ticks,
                      value.m_roarDue, value.m_roared, value.m_brandished, value.m_thrown,
                      value.m_wearLeft);
    }
    bool operator==(const LegendRite&) const = default;
    Stage m_stage = Stage::None;
    LegendWeakness m_weakness;
    s32 m_player = -1;
    s32 m_ticks = 0; ///< since the boss rose
    bool m_roarDue = false;
    bool m_roared = false;
    bool m_brandished = false;
    bool m_thrown = false;
    f32 m_wearLeft = 0.0f; ///< seconds of the curb left, once the roar is over
};

/** When a legend item's strike falls on its boss. */
enum class LegendStrike : u8 {
    OnThrow,   ///< as the bearer throws it
    OnLanding, ///< as it lands: the scimitar and javelin on impact, bellows and savior on release
    AtRiteEnd  ///< as its landing ends the rite (LegendRite::finishOnImpact): the axe and lamp
};

/**
 * How the rite looks and sounds, as the original stages it: the item held (`LEGENDHLD`)
 * glows in the bearer's hand, or over their head for the bosses with no hand to hold it
 * at; the bearer's gesture is the strong throw, the special shot or a potion's use by the
 * boss's kind; and the item (`LEGENDPRJ`) flies at the boss, is set on it, or rides ahead
 * of the bearer, giving way to its burst (`LEGENDFX`, or `LEGENDFX2` after it) for a while.
 * Its sounds are the realm's: picked up, thrown, flying, landed, and worn off.
 */
struct LegendShow {
    /** What the item does once let go of. */
    enum class Flight : u8 {
        Flies,     ///< to the boss, at `kSpeed`
        AtBoss,    ///< set where the boss stands, offset by `bossOffsetOf`
        WithBearer ///< ahead of the bearer, riding with them
    };
    /** A moment of the rite with a sound of its own. */
    using Sound = LegendSound;

    static constexpr std::string_view kHeldTree = "LEGENDHLD";
    static constexpr std::string_view kProjectileTree = "LEGENDPRJ";
    static constexpr std::string_view kBurstTree = "LEGENDFX";
    static constexpr std::string_view kSecondBurstTree = "LEGENDFX2";
    static constexpr f32 kHeldLift = 8.0f;         ///< over the bearer, held with no hand
    static constexpr f32 kHeldSeconds = 999999.0f; ///< held until let go of, as the original
    static constexpr f32 kSpeed = 20.0f;           ///< units a second, flying
    static constexpr f32 kLift = 2.0f;             ///< the flight starts this high
    static constexpr f32 kFlightSeconds = 6.0f;    ///< at most
    static constexpr f32 kAhead = 5.0f;            ///< the bearer's, ahead of them
    static constexpr f32 kBurstSeconds = 30.0f;
    static constexpr f32 kLichBurstSeconds = 5.0f;
    static constexpr f32 kSpiderBurstSeconds = 3.0f;
    static constexpr f32 kGenieBurstSeconds = 28.0f; ///< the lamp's burst, riding the genie
    static constexpr std::string_view kAuraTree = "COMBO_SPH";
    static constexpr f32 kBurstPlaybackRate = 1.0f / 0.333f;

    /** The coloured charge-up burst and sphere tint selected by costume colour. */
    static std::string_view chargeTree(s32 color);
    static Color chargeTint(s32 color);
    /** The code-created trail on the dragon's axe, chimera's scimitar and plague vial;
     * an empty texture means the item has no such trail. */
    static ParticleDescriptor trailOf(s32 kind);

    /** Whether the held item is in the bearer's hand, not over their head. */
    static bool heldInHand(s32 kind);
    static PlayerDeed gestureOf(s32 kind);
    static Flight flightOf(s32 kind);
    /** The tree the item is set on the boss as, or rides ahead of the bearer as. */
    static std::string_view restingTreeOf(s32 kind);
    /** The tree that takes over once the item has played, and how long it lasts. */
    static std::string_view burstTreeOf(s32 kind);
    static f32 burstSecondsOf(s32 kind);
    /** Where on the boss the item is set. */
    static Vec3 bossOffsetOf(s32 kind);

    /** When the item's strike falls on the boss. */
    static LegendStrike strikeOf(s32 kind);
    /** Whether a landing strikes only while the rite still runs. */
    static bool landsWhileRiteRuns(s32 kind);
    /** Whether the item lands as the bearer's cast is released. */
    static bool landsOnRelease(s32 kind);
    /** Whether the boss roars a second after rising, not three. */
    static bool quickToRoar(s32 kind);
    /** Whether the rite's darkness outlasts the roar, lifting only as the item lands. */
    static bool darkUntilImpact(s32 kind);
    /** Whether the weakness wearing off ends the boss's blindness and gives back its eye,
     * with none of the other items' sounds (world_update, 0x8005674c). */
    static bool restoresSight(s32 kind);
    /** Whether the flying item steers at its moving target each update for its whole
     * flight, rather than being sent straight at it (ProcessEffects' 0x40000000 branch). */
    static bool homes(s32 kind);
    /** The mesh the strike puts on the boss's `EYEBALL` node; empty for none. */
    static std::string_view eyeReplacementOf(s32 kind);
    /** The tint the strike leaves on the boss, if any. */
    static std::optional<Color> strikeTintOf(s32 kind);
    /** The tree shown for the boss's `STUMPL` once the rite has begun; empty for none. */
    static std::string_view lionStumpOf(s32 kind);
    /** The boss's node the item is aimed at; empty for none. */
    static std::string_view targetNodeOf(s32 kind);
    /** The item archive's texture the boss wears once the item lands; empty for none. */
    static std::string_view frozenSkinOf(s32 kind);
    /** How long the landed burst repeats, riding on the boss; nought: it plays once where
     * it struck. */
    static f32 burstFollowSecondsOf(s32 kind);

    /** The names the realm's sound for the moment may go by, first the likeliest. */
    static std::vector<std::string> soundNamesOf(Sound sound, char realm);
};

} // namespace gdl::game
