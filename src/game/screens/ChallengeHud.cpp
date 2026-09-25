#include "game/screens/ChallengeHud.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <span>

#include "engine/core/Log.h"

namespace gdl::game {
namespace {
constexpr f32 kSize = 128;
constexpr f32 kUpperTop = 23;
constexpr f32 kUpperHeight = 41;
constexpr f32 kUpperTravel = 39;
constexpr f32 kLowerTop = 105;
constexpr f32 kLowerHeight = 23;
constexpr f32 kLowerTravel = 38;
constexpr f32 kAnimationRate = 30;
} // namespace

bool ChallengeHud::bind(RenderDevice& device, ItemArchive& archive) {
    clear();
    const auto frame = archive.textures.find("TIMER");
    const auto sand = archive.textures.find("TIMER_SAND");
    if (!frame || !sand) {
        return false;
    }
    try {
        m_frame = &archive.textures.texture(device, *frame);
        m_sand = &archive.textures.texture(device, *sand);
        for (const auto& animation : archive.trees.textureAnimations()) {
            if (animation.name == "SAND_ANIM") {
                m_falling.bind(std::span{&animation, usize{1}}, archive.textures, device);
                break;
            }
        }
        return true;
    } catch (const std::exception& error) {
        log::warn("Challenge hourglass: {}", error.what());
        clear();
        return false;
    }
}

void ChallengeHud::clear() {
    m_frame = nullptr;
    m_sand = nullptr;
    m_falling.clear();
    m_frames = 0;
}

void ChallengeHud::step(f32 seconds) {
    m_frames += std::max(seconds, 0.0f) * kAnimationRate;
    const auto frames = static_cast<u32>(m_frames);
    m_frames -= static_cast<f32>(frames);
    m_falling.step(frames);
}

ChallengeHud::Sand ChallengeHud::sand(f32 remaining, f32 duration) {
    const f32 elapsed = duration > 0 ? std::clamp(1 - remaining / duration, 0.0f, 1.0f) : 1;
    const f32 upperTravel = std::round(kUpperTravel * elapsed);
    const f32 lowerTravel = std::round(kLowerTravel * elapsed);
    const f32 upperUv = (kUpperTop + kUpperHeight * elapsed) / kSize;
    const f32 lowerUv = (kLowerTop - kLowerTravel * elapsed) / kSize;
    return {{1, kUpperTop + 1 + upperTravel, kSize, kUpperHeight - upperTravel},
            {0, upperUv, 1, 0.5f - upperUv},
            {1, kLowerTop + 1 - lowerTravel, kSize, kLowerHeight + lowerTravel},
            {0, lowerUv, 1, 1 - lowerUv}};
}

void ChallengeHud::draw(Canvas& canvas, f32 remaining, f32 duration, bool running) const {
    if (m_frame == nullptr || m_sand == nullptr) {
        return;
    }
    canvas.draw(*m_frame, {1, 1, kSize, kSize});
    if (running && m_falling.size() != 0) {
        if (const auto* frame = m_falling.motion(0).frame) {
            constexpr Rect kFalling{63, 58, 8, 32};
            canvas.draw(*frame, kFalling);
        }
    }
    const auto shape = sand(remaining, duration);
    canvas.draw(*m_sand, shape.upper, shape.upperUv, Color::white());
    canvas.draw(*m_sand, shape.lower, shape.lowerUv, Color::white());
}

} // namespace gdl::game
