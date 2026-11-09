#pragma once

#include <optional>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {

/** Local imported target identity. The gameplay bridge supplies the shared
 * attack-system ID namespace; this lower-level module does not depend on it. */
struct LegendsContainerTarget {
    s32 id = -1;
    Vec3 base{0};
    f32 radius = 0;
    f32 height = 0;
    bool breakable = false;
    u32 nativeIndex = 0;
    bool nativeTrap = false;
};

/** A barrel or chest resolved from either platform's native records. What it
 * holds stays with the platform that binds it. */
struct LegendsContainerDefinition {
    enum class Kind { Barrel, Chest };
    Kind kind = Kind::Barrel;
    u32 nativeIndex = 0;
    Vec3 position{0};
    f32 yaw = 0;
    Obstacle closedObstacle;
    f32 damageThreshold = 0;
    std::string closed;
    std::string opening;
    std::string broken;
    std::string goldOpening;
    std::optional<u16> selector;
    std::string diagnostic;
    enum class Hazard { None, Fire, Poison };
    Hazard hazard = Hazard::None;
    std::string group = "CONTAINER";
    u32 openingSound = 0;
    f32 blastRadius = 0;
    f32 blastDamage = 0;
    f32 scale = 1;
    bool interactive = true; ///< false retains a deferred native container's solid body
    enum class DamageMode { Threshold, Accumulated };
    DamageMode damageMode = DamageMode::Threshold;
    s32 armor = 0;
    Vec3 blastOffset{0, 2, 0};
};

/** Container rules use ordinary GDL attacks and inventory capacity. A native N64
 * barrel needs one sufficient blow; a Dreamcast one accumulates the blows that get
 * past its armor (DamageMode). Presentation completion, not a guessed wall-clock
 * duration, releases loot. */
class LegendsContainer {
public:
    enum class State { Closed, Opening, Open, Empty };
    explicit LegendsContainer(LegendsContainerDefinition definition);
    const LegendsContainerDefinition& definition() const { return m_definition; }
    State state() const { return m_state; }
    bool targetable() const;
    bool strike(f32 power);
    /** Spend a key only on the first genuine contact with a shut chest. */
    bool touch(const Vec3& feet, f32 radius, f32 height, s32& keys);
    bool finishOpening();
    void collected();
    /** Carry the complete container with a native floor owner. */
    void placeAt(const Vec3& feet);
    /** Open containers do not trap a player whose inventory refuses their loot. */
    Obstacle obstacle() const;

private:
    LegendsContainerDefinition m_definition;
    State m_state = State::Closed;
    s32 m_health = 0;
};

} // namespace gdl::game
