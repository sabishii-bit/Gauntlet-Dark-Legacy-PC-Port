#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"
#include "engine/world/ParticleSystem.h"

#include "game/players/PlayerAnimator.h"

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
    int boss = -1;            ///< the boss kind, 34 the dragon to 44 the garm
    int realm = 0;            ///< the item, by the realm whose boss this is
    float healthShare = 0.0f; ///< of its health now, taken at once
    float damage = 0.0f;      ///< or this much, taken at once
    int frozenTicks = 0;      ///< it stands frozen this long
    int blindTicks = 0;       ///< it loses its targets and turns at a tenth this long
    float curbSeconds = 0.0f; ///< over nought its curbed attacks are refused, bursts cut to this
    float curbLasts = 0.0f;   ///< seconds the curb lasts, from its roar; for good when nought
    float scale = 1.0f;       ///< how big it stands afterwards
    bool beheads = false;     ///< the item takes one of its heads (a third of it, for now)

    bool harms() const { return healthShare > 0.0f || damage > 0.0f; }
    bool curbs() const { return curbSeconds > 0.0f; }
};

/** The weakness of the boss `kind`, or null for one no item touches. */
const LegendWeakness* legendWeaknessOf(int kind);
/** The realm of the legend item that weakens the boss `kind`, or nought. */
int legendRealmOf(int kind);

/** Something the rite did that the game shows. */
enum class LegendCue : std::uint8_t {
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
    enum class Stage : std::uint8_t { None, Carried, Woken, Struck, Over };

    static constexpr int kBrandishTicks = 60;
    static constexpr int kShortRoarWait = 60;
    static constexpr int kLongRoarWait = 180;

    /** Begins for `player`, who carries the item of `weakness`. */
    void begin(int player, const LegendWeakness& weakness);
    void clear();

    /** Moves the rite `ticks` on, told whether the boss has finished rising and whether it
     * has finished roaring; the cues to show come back. */
    std::vector<LegendCue> update(int ticks, bool bossRisen, bool bossRoarDone);
    /** The Dragon's projectile hit advances retail state 2/3 to 4 without waiting for a
     * roar; its twenty-second freeze is independent of the presentation's darkness. */
    bool finishOnImpact();

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Over; }
    int player() const { return m_player; }
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
    bool darkens() const {
        return running() && m_stage != Stage::Carried && (m_weakness.boss == 34 || !m_roared);
    }
    static constexpr float kDarkening = -0.8f; ///< what it takes off the ambient light

private:
    Stage m_stage = Stage::None;
    LegendWeakness m_weakness;
    int m_player = -1;
    int m_ticks = 0; ///< since the boss rose
    bool m_roarDue = false;
    bool m_roared = false;
    bool m_brandished = false;
    bool m_thrown = false;
    float m_wearLeft = 0.0f; ///< seconds of the curb left, once the roar is over
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
    enum class Flight : std::uint8_t {
        Flies,     ///< to the boss, at `kSpeed`
        AtBoss,    ///< set where the boss stands, offset by `bossOffsetOf`
        WithBearer ///< ahead of the bearer, riding with them
    };
    /** A moment of the rite with a sound of its own. */
    enum class Sound : std::uint8_t { PickedUp, Thrown, Flying, Landed, WornOff };

    static constexpr std::string_view kHeldTree = "LEGENDHLD";
    static constexpr std::string_view kProjectileTree = "LEGENDPRJ";
    static constexpr std::string_view kBurstTree = "LEGENDFX";
    static constexpr std::string_view kSecondBurstTree = "LEGENDFX2";
    static constexpr float kHeldLift = 8.0f;         ///< over the bearer, held with no hand
    static constexpr float kHeldSeconds = 999999.0f; ///< held until let go of, as the original
    static constexpr float kSpeed = 20.0f;           ///< units a second, flying
    static constexpr float kLift = 2.0f;             ///< the flight starts this high
    static constexpr float kFlightSeconds = 6.0f;    ///< at most
    static constexpr float kAhead = 5.0f;            ///< the bearer's, ahead of them
    static constexpr float kBurstSeconds = 30.0f;
    static constexpr float kLichBurstSeconds = 5.0f;
    static constexpr float kSpiderBurstSeconds = 3.0f;
    static constexpr std::string_view kAuraTree = "COMBO_SPH";
    static constexpr float kBurstPlaybackRate = 1.0f / 0.333f;

    /** The coloured charge-up burst and sphere tint selected by costume colour. */
    static std::string_view chargeTree(int color);
    static Color chargeTint(int color);
    /** The code-created trail on the dragon's axe, chimera's scimitar and plague vial;
     * an empty texture means the item has no such trail. */
    static ParticleDescriptor trailOf(int kind);

    /** Whether the held item is in the bearer's hand, not over their head. */
    static bool heldInHand(int kind);
    static PlayerDeed gestureOf(int kind);
    static Flight flightOf(int kind);
    /** The tree the item is set on the boss as, or rides ahead of the bearer as. */
    static std::string_view restingTreeOf(int kind);
    /** The tree that takes over once the item has played, and how long it lasts. */
    static std::string_view burstTreeOf(int kind);
    static float burstSecondsOf(int kind);
    /** Where on the boss the item is set. */
    static Vec3 bossOffsetOf(int kind);
    /** The names the realm's sound for the moment may go by, first the likeliest. */
    static std::vector<std::string> soundNamesOf(Sound sound, char realm);
};

} // namespace gdl::game
