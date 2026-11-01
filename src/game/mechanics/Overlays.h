#pragma once

#include <span>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** A living body of the swarm, for presentation drawn over it. */
struct SwarmBodyView {
    u64 instance = 0; ///< the body's lifetime in its pool slot, never reused
    s32 affinity = 0; ///< `BodyTraits::affinity`
    Vec3 feet{0.0f};
    f32 radius = 0;
    f32 height = 0;
};

/** What a world overlay draws with, in the 3D pass after the swarm's effects. */
struct WorldOverlayView {
    RenderDevice& device;
    const Mat4& clip;
    const CameraFrame& camera;
    bool paused = false;                   ///< play is held behind a menu: time stands
    std::span<const SwarmBodyView> bodies; ///< the living swarm
    ItemArchive* weapons = nullptr;        ///< the shared weapons archive, when loaded
};

/** One local seat's run of close blows, for presentation. */
struct SeatRunView {
    s32 player = 0;
    s32 count = 0;
    f32 quiet = 0; ///< seconds since its last blow
    bool running = false;
    s32 record = 0; ///< the character's longest run so far
};

/** What a HUD overlay draws with, on the 2D canvas over the status boxes. */
struct HudOverlayView {
    Canvas& canvas;
    const TextPainter& text;
    const Texture* glow = nullptr; ///< the glowing large font, when loaded
    const StringTable* strings = nullptr;
    std::span<const SeatRunView> runs; ///< this machine's seats still in the game
};

/** Presentation a mechanic draws in the world. Owns its own frame clock; never simulation. */
class WorldOverlay {
public:
    virtual ~WorldOverlay() = default;
    virtual void drawWorld(const WorldOverlayView& view) = 0;
};

/** Presentation a mechanic draws on the HUD. Owns its own frame clock; never simulation. */
class HudOverlay {
public:
    virtual ~HudOverlay() = default;
    virtual void drawHud(const HudOverlayView& view) = 0;
};

} // namespace gdl::game
