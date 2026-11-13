#include "game/party/PartyTeleport.h"

namespace gdl::game {

void PartyTeleport::sparkle(RenderDevice& device, EffectTrees& effects, ItemArchive& weapons,
                            std::span<const PlayerRuntime> players) {
    if (!weapons.loaded()) {
        return;
    }
    for (const PlayerRuntime& runtime : players) {
        if (!runtime.departed && runtime.life == PlayerLife::Standing) {
            effects.start(device, weapons, kEffect, runtime.actor.position());
        }
    }
}

} // namespace gdl::game
