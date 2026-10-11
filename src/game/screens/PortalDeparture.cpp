#include "game/screens/PortalDeparture.h"

#include <algorithm>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
void PortalDeparture::begin(RenderDevice& device, TextureSet& weapons) {
    clear();
    m_started = true;
    loadSkin(device, weapons);
}

void PortalDeparture::loadSkin(RenderDevice& device, TextureSet& weapons) {
    m_frames.fill(nullptr);
    for (usize i = 0; i < m_frames.size(); ++i) {
        const auto name = i == 0 ? "DTH_LIGHT00" : std::format("DTH_LIGHT00+{}", i);
        if (const auto index = weapons.find(name)) {
            try {
                m_frames[i] = &weapons.texture(device, *index);
            } catch (const std::exception& e) {
                log::warn("Portal skin: {}", e.what());
            }
        }
    }
}

void PortalDeparture::clear() {
    m_frames.fill(nullptr);
    m_ticks = 0;
    m_started = false;
}

void PortalDeparture::update(s32 ticks) {
    if (m_started) {
        m_ticks = std::min(m_ticks + std::max(ticks, 0), kTicks);
    }
}

Mat4 PortalDeparture::transform(const Mat4& body) const {
    if (!m_started) {
        return body;
    }
    return transformAt(body, phase());
}

Mat4 PortalDeparture::transformAt(const Mat4& body, f32 phase) {
    const f32 elapsed = std::clamp(phase, 0.0f, 1.0f) * kTicks;
    Mat4 result = glm::rotate(body, kSpinPerSecond * elapsed / 60.0f, Vec3{0, 1, 0});
    result[3] += Vec4{0, -elapsed * kSinkPerTick, 0, 0};
    return result;
}

const Texture* PortalDeparture::skin() const {
    return m_started ? skinAt(phase()) : nullptr;
}

const Texture* PortalDeparture::skinAt(f32 phase) const {
    if (phase < 0 || phase >= 1) {
        return nullptr;
    }
    // Skin rate 0.4 per 30 Hz frame, independently of the texture's free-running rate.
    const auto frame = static_cast<usize>(phase * kTicks * 0.2f);
    return m_frames[std::min(frame, m_frames.size() - 1)];
}
} // namespace gdl::game
