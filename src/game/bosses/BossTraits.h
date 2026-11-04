#pragma once

#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/bosses/BossDefinition.h"
#include "game/bosses/LegendItems.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::game {

/** How a boss's legend rite is staged, read through LegendShow's lookups. The defaults are
 * what a code without a row gets. A legend profile overrides some columns where they are
 * used; each such column names the profile field that does. */
struct LegendStaging {
    bool quickToRoar = false; ///< it roars a second after rising, not three
    LegendStrike strike = LegendStrike::OnThrow;
    bool landsWhileRiteRuns = false; ///< a landing strikes only while the rite still runs
    bool landsOnRelease = false;     ///< the item lands as the bearer's cast is released
    bool darkUntilImpact = false;    ///< the rite's darkness lifts only as the item lands
    bool heldInHand = false;         ///< held in the bearer's hand, not over their head
    PlayerDeed gesture = PlayerDeed::HurlLegend; ///< ShootLegend, ThrowLegend or HurlLegend
    LegendShow::Flight flight = LegendShow::Flight::AtBoss; ///< profile: flight
    bool homes = false; ///< the flying item steers at its moving target for its whole flight
    /** The tree set on the boss or riding with the bearer; profile: projectile. */
    std::string_view restingTree = LegendShow::kProjectileTree;
    /** The tree that takes over once the item has played; profile: secondBurst. */
    std::string_view burstTree = LegendShow::kBurstTree;
    f32 burstSeconds = LegendShow::kBurstSeconds;
    f32 burstFollowSeconds = 0.0f;     ///< the landed burst rides the boss this long; nought: not
    Vec3 bossOffset{0.0f, 0.0f, 0.0f}; ///< where on the boss the item is set; profile: bossOffset
    std::string_view trailTexture;     ///< the flight's particle trail; empty: none; profile: trail
    std::string_view targetNode;       ///< the node aimed at; empty: none; profile: targetNode
    std::string_view eyeReplacement;   ///< its struck EYEBALL; empty: none; profile: eyeReplacement
    bool restoresSight = false; ///< wearing off ends its blindness and gives back its eye, silently
    std::string_view lionStump; ///< the tree shown for its STUMPL; empty: none; profile: lionStump
    std::optional<Color> strikeTint; ///< the tint the strike leaves on it
    std::string_view frozenSkin;     ///< the item archive's texture it wears once the item lands

    bool operator==(const LegendStaging&) const = default;
};

/** Which rune line the wizard speaks after a boss's defeat, by how many of the realm's
 * runestones the party found (AudioGoodWizard's table; an S_RUNEVOX line ends in the realm's
 * letter). */
enum class RuneVoices : u8 {
    SomeIsAll, ///< S_RUNEVOX0 for none found, S_RUNEVOX1 for some and for all
    OwnAll,    ///< S_RUNEVOX0 for none, S_RUNEVOX1 for some, S_RUNEVOX2 for all
    Temple,    ///< S_E2VOXB whatever was found
    None,      ///< no rune line
};

/** What follows the wizard's defeat speech. */
enum class VictoryFollowUp : u8 {
    Realm,       ///< the realm's rune phrase, by how many of its runestones were found
    TempleRunes, ///< SKORNE1_RUNE_YES or _NO, by the party's first twelve runes
    Leaves,      ///< nothing: the party is taken out
};

/** What a boss's death move throws about its fall. */
enum class BossSpew : u8 {
    Coins,  ///< the realm's coins for each player (BossCoins)
    Relics, ///< the temple's relics (SkorneRelics)
};

/** What a boss's defeat says, and who comes for the party. */
struct BossDefeat {
    std::string_view speech; ///< the wizard's *_SPEECH message
    // NOLINTNEXTLINE(readability-redundant-member-init) -- default for partial aggregates
    std::string_view defeatVoice{}; ///< empty: S_DEFEATVOX and the realm's letter
    RuneVoices runeVoices = RuneVoices::SomeIsAll;
    VictoryFollowUp followUp = VictoryFollowUp::Realm;
    bool longWizardWait = false; ///< BossVictory::kLongWaitTicks, not kWaitTicks
    bool wizardAtAltar = false;  ///< DoGoodWizard's fixed altar spot, not the midpoint
    bool dropsKey = false;       ///< the BOSSKEY tree and S_BOSSKEY rise where it fell
    BossSpew spew = BossSpew::Coins;
};

/** One boss's facts above its stage. Adding a boss also takes a kWeaknesses row
 * (LegendItems.cpp), a BossStage row (level/BossKinds.cpp) and bossDefinition's family. */
struct BossTraits {
    s32 kind = -1;
    BossGeneratorVisual generator;    ///< its eggs' generator art in its own archive
    bool projectileEndVisual = false; ///< read by bossDefinition through its name
    LegendStaging legend;
    BossDefeat defeat;
};

/** The row of the boss a stored code names, or null for -1 and every other code. */
[[nodiscard]] const BossTraits* bossTraitsOf(s32 code);
/** The row of `code`, or of the boss code nearest it: the rules the original states as ordered
 * comparisons of the code (< 42, >= 38, > 42) give every code below the dragon's what they give
 * the dragon's, and every code above the garm's what they give the garm's. Rules stated as
 * equalities read bossTraitsOf. */
[[nodiscard]] const BossTraits& orderedBossTraitsOf(s32 code);
/** The legend staging of `code`'s row, or the defaults for a code without one. */
[[nodiscard]] const LegendStaging& legendStagingOf(s32 code);

} // namespace gdl::game
