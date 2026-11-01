#pragma once

#include <optional>

#include "engine/core/Types.h"

namespace gdl::game {

/** What landed a blow, as the base game knows it. */
enum class BlowSource : u8 {
    Swing,     ///< a melee swing
    Shot,      ///< a thrown or fired weapon
    SuperShot, ///< the super shot
    Potion,    ///< a potion's burst, cast or thrown, or its shield's ring
    Turbo,     ///< a turbo move's strike, the thunder hammer among them
    Charge,    ///< a charge or combo body blow
    Breath,    ///< a breath worn as a powerup
    Shield,    ///< a fire or lightning shield's touch
};

/** What a blow did to what it landed on. */
enum class BlowOutcome : u8 {
    Hurt,      ///< it stands, or was not standing to take it
    Killed,    ///< the blow felled it
    Destroyed, ///< a generator, to rubble
};

/** What a blow landed on. */
enum class BlowTarget : u8 {
    Enemy,   ///< one of the swarm taller than the short ones
    Critter, ///< a great one
    Boss,
    Generator,
};

/** What a blow does for the striker's run of close blows (`MeleeStreak`). */
enum class RunEffect : u8 {
    Nothing,
    Count,           ///< counts one
    CountAsOriginal, ///< counts as the original's swing did: one, or three when telling
};

/** A landed blow as the rules judge it, gathered by whoever struck. */
struct BlowReport {
    BlowSource source = BlowSource::Swing;
    BlowOutcome outcome = BlowOutcome::Hurt;
    BlowTarget target = BlowTarget::Enemy;
    bool timeStopped = false;  ///< the swarm stands still under a time stop
    bool invulnerable = false; ///< the striker cannot be harmed
    bool invisible = false;    ///< the striker cannot be seen
};

/** Rules about blows a mechanic may lay over the original's. */
class BlowRules {
public:
    virtual ~BlowRules() = default;
    /** What `blow` does for the striker's run; nothing to say keeps `originalRunEffect`. */
    virtual std::optional<RunEffect> runEffect(const BlowReport& blow) const = 0;
    /** Whether a blow of `source` harms a generator at all. */
    virtual bool generatorTakes(BlowSource source) const = 0;
};

/** The original's rule (player.c 2553): its swings on the swarm, the great ones and the
 * boss count, and nothing else does. */
constexpr RunEffect originalRunEffect(const BlowReport& blow) {
    return blow.source == BlowSource::Swing && blow.target != BlowTarget::Generator
               ? RunEffect::CountAsOriginal
               : RunEffect::Nothing;
}

} // namespace gdl::game
