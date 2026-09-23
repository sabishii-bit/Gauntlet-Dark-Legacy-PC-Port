#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
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
    static constexpr int kSpawnTicks = 60;

    /** Starts at fixed party positions; without a marker the follow camera is used. */
    void begin(RenderDevice& device, ItemArchive& weapons, std::span<const Vec3> positions,
               const std::optional<WorldCamera>& marker = std::nullopt);
    void clear();

    /** Advance visuals before the world and its listener update. */
    void animate(float seconds);
    /** Advance the hold and camera after the listener update, preserving its frame phase. */
    void advance(int ticks, bool skip, const Vec3& followPosition, const Vec3& followAttention);
    void drawEffects(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void drawTitle(Canvas& canvas, const TextPainter& text, std::string_view title,
                   float width) const;

    bool active() const { return m_camera.active() || m_ticks > 0; }
    const StartCamera& camera() const { return m_camera; }
    std::size_t effectCount() const { return m_spawns.size(); }

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
    int m_ticks = 0;
    float m_frames = 0.0f;
    StartCamera m_camera;
    float m_titleSlide = 0.0f;
};

} // namespace gdl::game
