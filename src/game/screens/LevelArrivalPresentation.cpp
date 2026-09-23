#include "game/screens/LevelArrivalPresentation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"

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
    m_frames = 0.0f;
    m_camera.stop();
    m_titleSlide = 0.0f;
}

void LevelArrivalPresentation::begin(RenderDevice& device, ItemArchive& weapons,
                                     std::span<const Vec3> positions,
                                     const std::optional<WorldCamera>& marker) {
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
        m_camera.start(*marker, centre);
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

void LevelArrivalPresentation::animate(f32 seconds) {
    if (!active()) {
        return;
    }
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const f32 whole = std::floor(m_frames);
    m_frames -= whole;
    if (whole > 0.0f) {
        m_textures.step(static_cast<u32>(whole));
    }
    for (Spawn& spawn : m_spawns) {
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(), static_cast<s32>(spawn.player.frame()));
        }
        for (usize i = 0; i < m_textures.size(); ++i) {
            const TextureMotion motion = m_textures.motion(i);
            if (motion.frame != nullptr) {
                spawn.model.setTextureFrame(motion.slot, motion.frame);
            } else {
                spawn.model.setTextureOffset(motion.slot, motion.offset);
            }
        }
    }
}

void LevelArrivalPresentation::advance(s32 ticks, bool skip, const Vec3& followPosition,
                                       const Vec3& followAttention) {
    if (!active()) {
        return;
    }
    m_ticks = std::max(m_ticks - ticks, 0);
    m_camera.update(ticks, skip, followPosition, followAttention);
    m_titleSlide += kTitleSlideRate * static_cast<f32>(ticks);
    if (m_camera.phase() != StartCamera::Phase::Hold || m_titleSlide > kTitleSlideEnd) {
        m_titleSlide = kTitleSlideEnd;
    }
}

void LevelArrivalPresentation::drawEffects(RenderDevice& device, const Mat4& clip,
                                           const WorldLighting& lighting) const {
    if (m_ticks <= 0) {
        return;
    }
    for (const Spawn& spawn : m_spawns) {
        spawn.model.draw(device, clip, glm::translate(Mat4{1.0f}, spawn.position), lighting,
                         spawn.pose.matrices());
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
