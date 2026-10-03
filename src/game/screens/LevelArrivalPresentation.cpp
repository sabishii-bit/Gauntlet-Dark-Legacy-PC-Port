#include "game/screens/LevelArrivalPresentation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr std::string_view kSpawnEffect = "STARTFX";
constexpr s32 kTitleY = 48;
constexpr s32 kTitleLift = 16;
constexpr f32 kTitleSlideStart = 0.025f;
constexpr f32 kTitleSlideRate = 0.025f;
constexpr f32 kTitleSlideEnd = 2.0f;
} // namespace

void LevelArrivalPresentation::clear() {
    m_spawns.clear();
    m_textures.clear();
    m_ticks = 0;
    m_camera.stop();
    m_titleSlide = 0.0f;
    m_titleLanded = false;
}

void LevelArrivalPresentation::begin(RenderDevice& device, ItemArchive& weapons,
                                     std::span<const Vec3> positions,
                                     const std::optional<WorldCamera>& marker,
                                     StartCamera::Mode mode, std::optional<Vec3> cameraFocus) {
    clear();
    m_ticks = kSpawnTicks;
    m_titleSlide = kTitleSlideStart;
    if (marker.has_value()) {
        Vec3 centre{0.0f};
        for (const Vec3& position : positions) {
            centre += position;
        }
        if (!positions.empty()) {
            centre /= static_cast<f32>(positions.size());
        }
        m_camera.start(*marker, cameraFocus.value_or(centre), mode);
    }
    const auto tree = weapons.loaded() ? weapons.trees.find(kSpawnEffect) : std::nullopt;
    if (!tree.has_value()) {
        log::warn("Tower: no {} in WEAPONS; the party appears without it", kSpawnEffect);
        return;
    }
    const TreeInfo& effect = weapons.trees.tree(*tree);
    for (const Vec3& position : positions) {
        Spawn spawn;
        spawn.position = position;
        spawn.tree = &effect;
        if (!spawn.model.bind(effect, weapons.models, weapons.textures, device)) {
            continue;
        }
        if (!effect.sequences.empty()) {
            spawn.player.start(effect.sequences[0], 0);
            spawn.pose.evaluate(effect, 0, 0.0f);
            spawn.model.setFrame(0, 0);
        } else {
            spawn.pose.rest(effect);
        }
        m_spawns.push_back(std::move(spawn));
    }
    m_textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
}

void LevelArrivalPresentation::capturePresentation() {
    m_textures.advance(0);
    for (Spawn& spawn : m_spawns) {
        spawn.presentationAdvanced = false;
    }
}

void LevelArrivalPresentation::animate(f32 seconds) {
    if (!active()) {
        return;
    }
    m_textures.advance(seconds);
    for (Spawn& spawn : m_spawns) {
        spawn.previousFrame = spawn.player.presentationFrame();
        spawn.previousGeneration = spawn.player.generation();
        spawn.presentationAdvanced = seconds > 0;
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(), static_cast<s32>(spawn.player.frame()));
        }
        m_textures.apply(spawn.model, *spawn.tree, spawn.player.sequence(), spawn.player.frame());
    }
}

void LevelArrivalPresentation::advance(s32 ticks, bool skip, const Vec3& followPosition,
                                       const Vec3& followAttention) {
    if (!active()) {
        return;
    }
    m_ticks = std::max(m_ticks - ticks, 0);
    m_camera.update(ticks, skip, followPosition, followAttention);
    const bool sliding = m_titleSlide < kTitleSlideEnd;
    m_titleSlide += kTitleSlideRate * static_cast<f32>(ticks);
    if (m_camera.phase() != StartCamera::Phase::Hold || m_titleSlide > kTitleSlideEnd) {
        m_titleSlide = kTitleSlideEnd;
    }
    m_titleLanded = m_titleLanded || (sliding && m_titleSlide == kTitleSlideEnd);
}

void LevelArrivalPresentation::drawEffects(RenderDevice& device, const Mat4& clip,
                                           const WorldLighting& lighting, f32 frameBlend) const {
    if (m_ticks <= 0) {
        return;
    }
    for (const Spawn& spawn : m_spawns) {
        TreePose visualPose;
        const TreePose* pose = &spawn.pose;
        const f32 blend = frameBlend < 0 || spawn.presentationAdvanced ? frameBlend : 1.0f;
        f32 frame = spawn.player.frame();
        if (blend >= 0 && spawn.player.playing()) {
            frame = spawn.previousGeneration == spawn.player.generation()
                        ? std::lerp(spawn.previousFrame, spawn.player.presentationFrame(),
                                    std::clamp(blend, 0.0f, 1.0f))
                        : spawn.player.presentationFrame();
            visualPose.evaluate(*spawn.tree, spawn.player.sequence(), frame, false, true);
            pose = &visualPose;
        }
        spawn.model.setPresentationFrame(spawn.player.sequence(), frame);
        m_textures.apply(spawn.model, *spawn.tree, spawn.player.sequence(), frame,
                         m_textures.presentationOffset(blend));
        spawn.model.draw(device, clip, glm::translate(Mat4{1.0f}, spawn.position), lighting,
                         pose->matrices());
    }
}

void LevelArrivalPresentation::drawTitle(Canvas& canvas, const TextPainter& text,
                                         std::string_view title, f32 width) const {
    if (!active() || title.empty() || !text.ready()) {
        return;
    }
    const TextStyle style;
    const s32 y = kTitleY - static_cast<s32>(static_cast<f32>(kTitleLift) * m_titleSlide);
    text.draw(canvas, -static_cast<s32>(width / 2.0f), y, title, style);
}

} // namespace gdl::game
