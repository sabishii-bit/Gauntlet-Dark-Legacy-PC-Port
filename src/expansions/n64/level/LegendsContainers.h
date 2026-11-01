#pragma once

#include <optional>
#include <span>
#include <string>

#include "expansions/n64/formats/LegendsN64Scene.h"
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
    std::optional<formats::LegendsN64Fixture> contents;
    std::optional<u16> selector;
    std::string diagnostic;
    enum class Hazard { None, Fire, Poison };
    Hazard hazard = Hazard::None;
    std::string group = "CONTAINER";
    u32 openingSound = 0;
    f32 blastRadius = 0;
    f32 blastDamage = 0;
    f32 scale = 1;
};

/** Native PCon/Ppower records, not fabricated GameCube item definitions.
 * Unknown scripted contents remain explicit diagnostics, never guessed loot. */
std::optional<LegendsContainerDefinition>
decodeLegendsN64Container(const formats::LegendsN64Fixture& fixture, std::span<const u8> containers,
                          std::span<const u8> powerups);

/** Red and green explosive barrels are PTrap records, not PCon containers. */
std::optional<LegendsContainerDefinition>
decodeLegendsN64Explosive(const formats::LegendsN64Fixture& fixture, std::span<const u8> traps);

/** Container rules use ordinary GDL attacks and inventory capacity. Native N64
 * barrels require one sufficient blow, rather than accumulating a health pool.
 * Presentation completion, not a guessed wall-clock duration, releases loot. */
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
    /** Carry the complete container and its bound loot with a native floor owner. */
    void placeAt(const Vec3& feet);
    /** Open containers do not trap a player whose inventory refuses their loot. */
    Obstacle obstacle() const;

private:
    LegendsContainerDefinition m_definition;
    State m_state = State::Closed;
};

} // namespace gdl::game
