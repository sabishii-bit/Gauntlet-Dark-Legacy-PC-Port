#pragma once

#include <vector>

#include "engine/core/Types.h"

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
    s32 boss = -1;            ///< the boss kind, 34 the dragon to 44 the garm
    s32 realm = 0;            ///< the item, by the realm whose boss this is
    f32 healthShare = 0.0f;   ///< of its health now, taken at once
    f32 damage = 0.0f;        ///< or this much, taken at once
    s32 frozenTicks = 0;      ///< it stands frozen this long
    s32 blindTicks = 0;       ///< it loses its targets and turns at a tenth this long
    f32 curbSeconds = 0.0f;   ///< over nought its curbed attacks are refused, bursts cut to this
    f32 curbLasts = 0.0f;     ///< seconds the curb lasts, from its roar; for good when nought
    f32 scale = 1.0f;         ///< how big it stands afterwards
    bool beheads = false;     ///< the item takes one of its heads (a third of it, for now)

    bool harms() const { return healthShare > 0.0f || damage > 0.0f; }
    bool curbs() const { return curbSeconds > 0.0f; }
};

/** The weakness of the boss `kind`, or null for one no item touches. */
const LegendWeakness* legendWeaknessOf(s32 kind);
/** The realm of the legend item that weakens the boss `kind`, or nought. */
s32 legendRealmOf(s32 kind);

/** Something the rite did that the game shows. */
enum class LegendCue : u8 {
    Brandished, ///< the bearer holds the item up: it is theirs no more
    Thrown,     ///< they throw it, and it strikes
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

    /** Begins for `player`, who carries the item of `weakness`. */
    void begin(s32 player, const LegendWeakness& weakness);
    void clear();

    /** Moves the rite `ticks` on, told whether the boss has finished rising and whether it
     * has finished roaring; the cues to show come back. */
    std::vector<LegendCue> update(s32 ticks, bool bossRisen, bool bossRoarDone);

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Over; }
    s32 player() const { return m_player; }
    const LegendWeakness* weakness() const { return m_stage != Stage::None ? &m_weakness : nullptr; }
    /** Whether the boss holds its stance for the bearer. */
    bool holdsBoss() const { return m_stage == Stage::Carried || m_stage == Stage::Woken; }
    /** Whether the boss should roar now. */
    bool wantsRoar() const { return m_roarDue && !m_roared; }
    bool thrown() const { return m_thrown; }
    /** Whether its weakness is on the boss. */
    bool weakening() const { return m_thrown && m_stage != Stage::Over; }

private:
    Stage m_stage = Stage::None;
    LegendWeakness m_weakness;
    s32 m_player = -1;
    s32 m_ticks = 0;      ///< since the boss rose
    bool m_roarDue = false;
    bool m_roared = false;
    bool m_brandished = false;
    bool m_thrown = false;
    f32 m_wearLeft = 0.0f; ///< seconds of the curb left, once the roar is over
};

} // namespace gdl::game
