#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/characters/Party.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** How a party comes to a Legends map. */
enum class LegendsArrival : u8 {
    Console,   ///< the developer console's goto
    Departure, ///< the Legends world's own way on once it covers the view: a portal, a boss's
               ///< end, a completed level's journey
    Retreat,   ///< back to its Tower after Quit Level or the party's defeat
    Gateway,   ///< Sumner's gateway, once the departure through it covers the view
};

/** A Legends map to open, and how the party comes to it. */
struct LegendsOpening {
    LegendsPlatform platform = LegendsPlatform::N64;
    std::filesystem::path directory;
    LegendsArrival arrival = LegendsArrival::Console;
    bool playable = true; ///< walked; else flown through as the console's preview
    /** The party that comes, else the one in the scene it leaves. */
    std::optional<std::vector<PartyMember>> party = std::nullopt;
    bool completionRecorded = false; ///< a completed journey has recorded the map it left
};

/** What a Legends map's opening makes of the visit to the Legends campaigns and of the scene
 * it leaves. */
struct LegendsVisitStep {
    bool starts = false; ///< none was under way: the party opening it becomes the visit's
    bool parks = false;  ///< the scene left waits for the console's goto back; else it goes

    bool operator==(const LegendsVisitStep&) const = default;
};

/** The console's goto from a campaign level starts a visit and parks that level for goto back;
 * Sumner's gateway starts one and lets the level go, the party coming home through a gateway
 * too; within a visit every opening lets the map left go. */
LegendsVisitStep legendsVisitStep(LegendsArrival arrival, bool visitUnderWay);

/** How Dark Legacy's Tower opens for a party come home through Sumner's gateway: afresh, at
 * its entrance with an arrival's presentation, Sumner's welcome long over. */
PlayOptions gatewayHomeArrival();

} // namespace gdl::game
