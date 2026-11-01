#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/config/GameConfig.h"
#include "game/mechanics/GameplayMechanic.h"

namespace gdl::game {

/** What a mechanic is, as the catalog lists it: enough to offer it in the settings and an
 * online room, and to build one for each level. */
struct MechanicDescriptor {
    std::string_view id;    ///< stable lowercase name: its settings key and digest name
    u32 schema = 1;         ///< its native state's layout version
    std::string_view label; ///< text id of the name the settings and the room show
    bool roomRule = true;   ///< offered as a rule the host of an online room sets
    std::unique_ptr<GameplayMechanic> (*create)() = nullptr;
};

/**
 * Every mechanic this build carries, ordered by id, so that every peer of one build lists
 * them, offers them and digests them alike. Filled once at startup, then only read.
 */
class MechanicCatalog {
public:
    static constexpr usize kMostRoomRules = 8; ///< the room carries its rules in one byte
    static constexpr usize kMostIdBytes = 32;

    /** Adds `descriptor`; false for a duplicate or malformed one, or one rule too many. */
    bool add(const MechanicDescriptor& descriptor);
    std::span<const MechanicDescriptor> all() const { return m_descriptors; }
    const MechanicDescriptor* find(std::string_view id) const;
    bool empty() const { return m_descriptors.empty(); }

    /** The room-rule descriptors in bit order. */
    std::vector<const MechanicDescriptor*> roomRules() const;
    /** The bits a room may set: one a room rule. */
    u8 knownRoomBits() const;
    /** The room's rule bits for `options`. */
    u8 roomBits(const GameOptionsConfig& options) const;
    /** Whether `options` switch on the mechanic `id`; off when unsaid. */
    static bool enabled(const GameOptionsConfig& options, std::string_view id);

    /** Every id with its schema, one `id@schema` a line, for the online identity. */
    std::string roster() const;

private:
    std::vector<MechanicDescriptor> m_descriptors; ///< by id
};

/** Switches the mechanics in `options` as an online room with rule `bits` does: each of
 * `catalog`'s room rules as its bit says and every other mechanic off, so that nothing but
 * the room's rules sets one peer's play apart. Every mechanic is off without a catalog. */
void applyRoomRules(const MechanicCatalog* catalog, u8 bits, GameOptionsConfig& options);

/** Registers every mechanic compiled into this build. Defined by the build's generated mod
 * list, linked only into the programs; the game library declares and never calls it. */
void registerCompiledMechanics(MechanicCatalog& catalog);

} // namespace gdl::game
