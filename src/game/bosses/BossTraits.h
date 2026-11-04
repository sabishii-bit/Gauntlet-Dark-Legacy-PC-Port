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

/** One boss's facts above its stage. Adding a boss also takes a kWeaknesses row
 * (LegendItems.cpp), a BossStage row (level/BossKinds.cpp) and bossDefinition's family. */
struct BossTraits {
    s32 kind = -1;
    BossGeneratorVisual generator;    ///< its eggs' generator art in its own archive
    bool projectileEndVisual = false; ///< read by bossDefinition through its name
    LegendStaging legend;
};

/** The row of the boss a stored code names, or null for -1 and every other code. */
[[nodiscard]] const BossTraits* bossTraitsOf(s32 code);
/** The legend staging of `code`'s row, or the defaults for a code without one. */
[[nodiscard]] const LegendStaging& legendStagingOf(s32 code);

} // namespace gdl::game
