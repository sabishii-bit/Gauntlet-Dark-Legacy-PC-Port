#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>
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
               const std::optional<WorldCamera>& marker = std::nullopt,
               StartCamera::Mode mode = StartCamera::Mode::Standard,
               std::optional<Vec3> cameraFocus = std::nullopt);
    void clear();

    /** Start a scene tick even when another presentation holds materialisation. */
    void capturePresentation();

    /** Advance visuals before the world and its listener update. */
    void animate(f32 seconds);
    /** Advance the hold and camera after the listener update, preserving its frame phase. */
    void advance(s32 ticks, bool skip, const Vec3& followPosition, const Vec3& followAttention);
    void drawEffects(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     f32 frameBlend = -1.0f) const;
    void drawTitle(Canvas& canvas, const TextPainter& text, std::string_view title,
                   f32 width) const;
    f32 titleScale() const { return active() ? m_titleSlide : 0; }
    static void drawTitleAt(Canvas& canvas, const TextPainter& text, std::string_view title,
                            f32 width, f32 scale);

    bool active() const { return m_camera.active() || m_ticks > 0; }
    const StartCamera& camera() const { return m_camera; }
    usize effectCount() const { return m_spawns.size(); }
    struct EffectPresentation {
        Vec3 position{0};
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        u32 sequence = 0;
        f32 frame = 0;
        u64 generation = 0;
        f32 textureClock = 0;
    };
    /** Read-only native cursors; absent after the materialisation ends. */
    std::optional<EffectPresentation> effectPresentation(usize index) const;
    bool takeTitleLanded() { return std::exchange(m_titleLanded, false); }

private:
    /** One character's materialisation at its entry position. */
    struct Spawn {
        Vec3 position{0.0f};
        const TreeInfo* tree = nullptr;
        mutable TreeModel model;
        TreePose pose;
        AnimationPlayer player;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationAdvanced = false;
    };
    std::vector<Spawn> m_spawns;
    const ItemArchive* m_archive = nullptr;
    TextureAnimator m_textures;
    s32 m_ticks = 0;
    StartCamera m_camera;
    f32 m_titleSlide = 0.0f;
    bool m_titleLanded = false;
};

} // namespace gdl::game
