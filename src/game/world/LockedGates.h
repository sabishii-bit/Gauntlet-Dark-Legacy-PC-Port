#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/Chests.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {

/** What a gate did this update. */
struct GateEvent {
    enum class Kind : u8 { Unlocked, Refused };
    Kind kind = Kind::Unlocked;
    usize gate = 0;
    usize visitor = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's locked gates, worked the way the original works them: each is a gate item that
 * stands shut across the way until someone against it has a key to spend, then swings open
 * and, half a second into its opening, no longer bars anyone.
 */
class LockedGates {
public:
    static constexpr s32 kShut = 0; ///< the figure's sequences
    static constexpr s32 kOpening = 1;
    static constexpr s32 kOpen = 2;
    static constexpr s32 kPassableTicks = 30; ///< into its opening, from when it bars no one
    static constexpr f32 kRefusalSeconds = 2.5f;

    /** One gate. */
    struct Gate {
        s32 instance = -1;
        s32 state = kShut;
        s32 openingTicks = 0;
        f32 refusalLeft = 0.0f;
        s32 minPlayers = 0;
        bool shown = true;
        ItemFigure figure;
        Obstacle box;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision);
    void clear();
    usize size() const { return m_gates.size(); }
    const Gate& gate(usize index) const { return *m_gates[index]; }
    void setPlayerCount(s32 players);

    std::vector<GateEvent> update(s32 ticks, f32 seconds, std::span<const ChestVisitor> party);
    /** The boxes of the gates that still bar the way. */
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Gate>> m_gates;
};

} // namespace gdl::game
