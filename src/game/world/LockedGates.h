#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/Chests.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {

/** What a gate did this update. */
struct GateEvent {
    enum class Kind : std::uint8_t { Unlocked, Refused };
    Kind kind = Kind::Unlocked;
    std::size_t gate = 0;
    std::size_t visitor = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's locked gates, worked the way the original works them: each is a gate item that
 * stands shut across the way until someone against it has a key to spend, then swings open
 * and, half a second into its opening, no longer bars anyone.
 */
class LockedGates {
public:
    static constexpr std::int32_t kShut = 0; ///< the figure's sequences
    static constexpr std::int32_t kOpening = 1;
    static constexpr std::int32_t kOpen = 2;
    static constexpr std::int32_t kPassableTicks =
        30; ///< into its opening, from when it bars no one
    static constexpr float kRefusalSeconds = 2.5f;

    /** One gate. */
    struct Gate {
        std::int32_t instance = -1;
        std::int32_t state = kShut;
        std::int32_t openingTicks = 0;
        float refusalLeft = 0.0f;
        std::int32_t minPlayers = 0;
        bool shown = true;
        ItemFigure figure;
        Obstacle box;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision);
    void clear();
    std::size_t size() const { return m_gates.size(); }
    const Gate& gate(std::size_t index) const { return *m_gates[index]; }
    void setPlayerCount(std::int32_t players);

    std::vector<GateEvent> update(std::int32_t ticks, float seconds,
                                  std::span<const ChestVisitor> party);
    /** The boxes of the gates that still bar the way. */
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Gate>> m_gates;
};

} // namespace gdl::game
