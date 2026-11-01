#pragma once

#include <memory>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/mechanics/BirthRules.h"
#include "game/mechanics/BlowRules.h"
#include "game/mechanics/FallRules.h"
#include "game/mechanics/Overlays.h"
#include "game/mechanics/RunRules.h"
#include "game/mechanics/SwarmAppearance.h"
#include "game/mechanics/SwarmRules.h"

namespace gdl::game {

/** The level a mechanic opens into. */
struct MechanicLevel {
    bool tower = false;
};

/** A mechanic's saved simulation state, opaque to all but the mechanic that made it. */
class MechanicState {
public:
    virtual ~MechanicState() = default;
};

/**
 * An additive gameplay mechanic: rules, simulation state and presentation laid over the base
 * game through narrow facets, each offered or left to the original. Its simulation state is
 * values every peer builds alike from the level and the confirmed inputs, saved and digested
 * in full; its presentation keeps its own clocks and never feeds the simulation.
 */
class GameplayMechanic {
public:
    virtual ~GameplayMechanic() = default;
    /** Starts `level` from nothing, as every peer starts it. */
    virtual void open(const MechanicLevel& level) = 0;
    /** Lets go of presentation resources, before the level's device and archives go. */
    virtual void close() = 0;
    /** Switched on or off by the settings or the room. Switched off it brings nothing new,
     * though what it already did to the level stays. */
    virtual void enable(bool on) = 0;

    /** A blank state of this mechanic's own kind, for a snapshot to fill again and again. */
    virtual std::unique_ptr<MechanicState> makeState() const = 0;
    virtual void saveState(MechanicState& saved) const = 0;
    virtual bool restoreState(const MechanicState& saved) = 0;
    /** Appends the live state, or `saved` when given, to the agreement digest. */
    virtual bool appendState(NativeStateDigest& digest, const MechanicState* saved) const = 0;

    virtual BlowRules* blows() { return nullptr; }
    virtual RunRules* runs() { return nullptr; }
    virtual SwarmRules* swarm() { return nullptr; }
    virtual BirthRules* births() { return nullptr; }
    virtual FallRules* falls() { return nullptr; }
    virtual SwarmAppearance* appearance() { return nullptr; }
    virtual WorldOverlay* world() { return nullptr; }
    virtual HudOverlay* hud() { return nullptr; }
};

} // namespace gdl::game
