#include "game/screens/LevelArrivalPresentation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {
namespace {
constexpr std::string_view kSpawnEffect = "STARTFX";
constexpr std::int32_t kTitleY = 48;
constexpr std::int32_t kTitleLift = 16;
constexpr float kTitleSlideStart = 0.025f;
constexpr float kTitleSlideRate = 0.025f;
constexpr float kTitleSlideEnd = 2.0f;
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
            centre /= static_cast<float>(positions.size());
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

void LevelArrivalPresentation::animate(float seconds) {
    if (!active()) {
        return;
    }
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const float whole = std::floor(m_frames);
    m_frames -= whole;
    if (whole > 0.0f) {
        m_textures.step(static_cast<std::uint32_t>(whole));
    }
    for (Spawn& spawn : m_spawns) {
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(),
                                 static_cast<std::int32_t>(spawn.player.frame()));
        }
        for (std::size_t i = 0; i < m_textures.size(); ++i) {
            const TextureMotion motion = m_textures.motion(i);
            if (motion.frame != nullptr) {
                spawn.model.setTextureFrame(motion.slot, motion.frame);
            } else {
                spawn.model.setTextureOffset(motion.slot, motion.offset);
            }
        }
    }
}

void LevelArrivalPresentation::advance(std::int32_t ticks, bool skip, const Vec3& followPosition,
                                       const Vec3& followAttention) {
    if (!active()) {
        return;
    }
    m_ticks = std::max(m_ticks - ticks, 0);
    m_camera.update(ticks, skip, followPosition, followAttention);
    m_titleSlide += kTitleSlideRate * static_cast<float>(ticks);
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
                                         std::string_view title, float width) const {
    if (!active() || title.empty() || !text.ready()) {
        return;
    }
    const TextStyle style;
    const std::int32_t y =
        kTitleY - static_cast<std::int32_t>(static_cast<float>(kTitleLift) * m_titleSlide);
    text.draw(canvas, -static_cast<std::int32_t>(width / 2.0f), y, title, style);
}

} // namespace gdl::game
