#pragma once

#include "expansions/n64/formats/LegendsN64Gates.h"
#include "expansions/shared/level/LegendsGates.h"

namespace gdl::game {

/** Resolves a native IPR/PTrap key door into the shared gate's engine-space
 * collision and policy. A non-finite placement yaw is rejected. */
LegendsGateDefinition legendsN64Gate(const formats::LegendsN64Gate& native);

} // namespace gdl::game
