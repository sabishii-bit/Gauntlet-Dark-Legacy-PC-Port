#include "game/screens/TransitionScreen.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr std::string_view kStaticDirectory = "STATIC";
constexpr f32 kFullAlpha = 255.0f;

} // namespace

bool TransitionScreen::load(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    release();
    if (!m_textures.load(unpackedRoot / kStaticDirectory)) {
        log::warn("Transition screen: the static archive is not unpacked; it shows black");
        return false;
    }
    const auto index = m_textures.find(kTexture);
    if (!index.has_value()) {
        log::warn("Transition screen: no {} in the static archive; it shows black", kTexture);
        return false;
    }
    try {
        m_picture = &m_textures.texture(device, *index);
    } catch (const std::exception& e) {
        log::warn("Transition screen: {}", e.what());
        return false;
    }
    return true;
}

void TransitionScreen::release() {
    m_picture = nullptr;
    m_textures.releaseTextures();
    m_phase = Phase::Off;
    m_opacity = 0.0f;
}

void TransitionScreen::comeUp() {
    if (m_phase != Phase::Covering) {
        m_phase = Phase::ComingUp;
    }
}

void TransitionScreen::cover() {
    m_phase = Phase::Covering;
    m_opacity = 1.0f;
}

void TransitionScreen::clearAway() {
    if (m_phase != Phase::Off) {
        m_phase = Phase::Clearing;
    }
}

void TransitionScreen::update(f32 seconds) {
    if (m_phase == Phase::ComingUp) {
        m_opacity = std::min(m_opacity + seconds / kFadeInSeconds, 1.0f);
        if (m_opacity >= 1.0f) {
            m_phase = Phase::Covering;
        }
    } else if (m_phase == Phase::Clearing) {
        m_opacity = std::max(m_opacity - seconds / kFadeOutSeconds, 0.0f);
        if (m_opacity <= 0.0f) {
            m_phase = Phase::Off;
        }
    }
}

void TransitionScreen::draw(Canvas& canvas, f32 width, f32 height, Area area) const {
    drawOpacity(canvas, width, height, showing() ? m_opacity : 0, area);
}

void TransitionScreen::drawOpacity(Canvas& canvas, f32 width, f32 height, f32 opacity,
                                   Area area) const {
    if (!(opacity > 0.0f)) {
        return;
    }
    // TransitionBlitShow selects 512x320 with live status boxes or 512x384 when
    // covering the whole screen. Menu loading has no live HUD beneath the picture.
    const Rect view{0.0f, 0.0f, width, area == Area::FullScreen ? height : kViewHeight};
    const auto alpha = static_cast<u8>(std::clamp(opacity * kFullAlpha, 0.0f, kFullAlpha));
    if (m_picture != nullptr) {
        canvas.draw(*m_picture, view, Color::white().withAlpha(alpha));
    } else {
        canvas.fill(view, Color::black().withAlpha(alpha));
    }
    // Keep the authored picture and status boxes at their original size, but
    // cover the additional world view with the very same fade on wide displays.
    canvas.maskOutside({0, 0, width, height}, Color::black().withAlpha(alpha));
}

} // namespace gdl::game
