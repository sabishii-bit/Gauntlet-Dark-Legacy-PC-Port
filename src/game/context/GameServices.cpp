#include "game/context/GameServices.h"

namespace gdl::game {

const GameServices& GameServices::none() {
    static const GameServices kNone;
    return kNone;
}

} // namespace gdl::game
