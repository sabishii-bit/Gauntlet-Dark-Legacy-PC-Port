#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldLighting.h"
#include "engine/world/WorldScene.h"

#include "game/world/ItemFigure.h"
#include "game/world/LevelTriggers.h"

namespace gdl::game {

/** What a rotator's object did this update, for the level to sound. */
struct RotatorCue {
    enum class Kind : u8 { Turning, Stopped };
    Kind kind = Kind::Turning;
    usize rotator = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's rotators (item type 12): world objects turned about their own upright axis at the
 * record's pace. One of subtype 0 turns for ever; one of subtype 2 is a bridge pad that waits
 * for someone to step on it, then turns its object through the record's angle and stops there
 * for good (the turntables that line bridges up).
 */
class Rotators {
public:
    static constexpr s32 kSpinning = 0;
    static constexpr s32 kTurnedByPad = 2;
    static constexpr f32 kTicksPerSecond = 60.0f; ///< the record's pace is a turn per tick
    static constexpr f32 kReach = LevelTriggers::kReach;

    struct Rotator {
        s32 instance = -1;
        s32 object = -1;
        s32 subtype = kSpinning;
        f32 speed = 0.0f; ///< radians a tick, the way YawMat3 turns
        f32 limit = 0.0f; ///< how far a pad's object turns, either way
        f32 turned = 0.0f;
        Vec3 origin{0.0f, 0.0f, 0.0f}; ///< the object's place under its parent
        Vec3 spot{0.0f, 0.0f, 0.0f};   ///< the pad, where it is stepped on
        f32 radius = 0.0f;
        bool started = false;
        bool done = false;
        std::unique_ptr<ItemFigure> pad;
    };

    void bind(const WorldLayout& layout);
    void bindFigures(RenderDevice& device, const WorldLayout& layout, ItemArchive& items);
    void clear() { m_rotators.clear(); }
    usize size() const { return m_rotators.size(); }
    const Rotator& rotator(usize index) const { return m_rotators[index]; }
    /** Turns what turns for `seconds`, starting pads that are stepped on. */
    std::vector<RotatorCue> update(f32 seconds, std::span<const TriggerVisitor> visitors,
                                   WorldScene& scene);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    static void place(const Rotator& rotator, WorldScene& scene);

    std::vector<Rotator> m_rotators;
};

} // namespace gdl::game
