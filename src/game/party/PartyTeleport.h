#pragma once

#include <span>
#include <string_view>

#include "engine/assets/ItemArchive.h"
#include "engine/render/RenderDevice.h"

#include "game/effects/EffectTrees.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {

/**
 * The good wizard's teleport as it takes a party away: DoGoodWizard (0x80019090) gives every
 * active player the teleport's skin effect through 0x800911C8 (kind 10) once its exit timer is
 * down to 0x80343B60's 35 ticks, shown here as WEAPONS' STARTFX at each member's feet. The boss's
 * fall and Sumner's gateways see a party off alike; each keeps its own clock.
 */
class PartyTeleport {
public:
    static constexpr std::string_view kEffect = "STARTFX";

    /** Starts the sparkle at the feet of every member standing and still in the game; nothing,
     * and no warning, without the WEAPONS archive loaded. */
    static void sparkle(RenderDevice& device, EffectTrees& effects, ItemArchive& weapons,
                        std::span<const PlayerRuntime> players);
};

} // namespace gdl::game
