#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldLighting.h"

#include "game/world/StartCamera.h"

namespace gdl::game {

/** The party's materialisation, opening camera and sliding level title. Borrows the
 * weapons archive; clear before releasing it. No players or world are retained. */
class LevelArrivalPresentation {
public:
    static constexpr s32 kSpawnTicks = 60;

    /** Starts at fixed party positions; without a marker the follow camera is used. */
    void begin(RenderDevice& device, ItemArchive& weapons, std::span<const Vec3> positions,
               const std::optional<WorldCamera>& marker = std::nullopt);
    void clear();

    /** Advance visuals before the world and its listener update. */
    void animate(f32 seconds);
    /** Advance the hold and camera after the listener update, preserving its frame phase. */
    void advance(s32 ticks, bool skip, const Vec3& followPosition, const Vec3& followAttention);
    void drawEffects(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void drawTitle(Canvas& canvas, const TextPainter& text, std::string_view title,
                   f32 width) const;

    bool active() const { return m_camera.active() || m_ticks > 0; }
    const StartCamera& camera() const { return m_camera; }
    usize effectCount() const { return m_spawns.size(); }

private:
    /** One character's materialisation at its entry position. */
    struct Spawn {
        Vec3 position{0.0f};
        const TreeInfo* tree = nullptr;
        TreeModel model;
        TreePose pose;
        AnimationPlayer player;
    };
    std::vector<Spawn> m_spawns;
    TextureAnimator m_textures;
    s32 m_ticks = 0;
    f32 m_frames = 0.0f;
    StartCamera m_camera;
    f32 m_titleSlide = 0.0f;
};

} // namespace gdl::game
