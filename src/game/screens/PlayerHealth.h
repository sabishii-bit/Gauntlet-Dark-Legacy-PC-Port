#pragma once
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>

#include "game/screens/PlayerRuntime.h"
namespace gdl::game {
/** What hurt a character, which picks how it cries out. */
enum class HurtKind : std::uint8_t {
    Blow,   ///< cries out once enough has been taken
    Burn,   ///< always cries out
    Pierce, ///< groans
    Gas     ///< coughs
};

/** Health/death rules and pain/low-health cue selection. Owns the party-wide cry RNG
 * and alternating last-health announcements; rendering and audio are synchronous outputs.
 * Like the scene it replaces, this cue sequence persists across level reopenings. */
class PlayerHealth {
public:
    struct Events {
        std::function<void(float, float)> block;
        std::function<void(std::string_view)> sound;
        std::function<void(std::string_view)> cry;
        std::function<void(std::string_view)> named;
    };
    void hurt(PlayerRuntime& runtime, float damage, HurtKind kind, bool directed, bool inTower,
              float damageScale, const Events& events);
    static float guarded(const PlayerRuntime& runtime, float damage, bool directed);

private:
    void cryPain(const Events& events);
    std::mt19937 m_painRandom{0x5A17u};
    std::uint32_t m_lowHealthTurn = 0;
};
} // namespace gdl::game
