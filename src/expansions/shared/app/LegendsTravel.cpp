#include "expansions/shared/app/LegendsTravel.h"

namespace gdl::game {

LegendsVisitStep legendsVisitStep(LegendsArrival arrival, bool visitUnderWay) {
    return {.starts = !visitUnderWay,
            .parks = arrival == LegendsArrival::Console && !visitUnderWay};
}

PlayOptions gatewayHomeArrival() {
    PlayOptions options;
    options.welcome = false;
    options.arriving = true;
    options.arrivalWorld = 0; // the entrance's start marker (towerMarkerOf), where a game begins
    return options;
}

} // namespace gdl::game
