#include "game/app/BuildLabel.h"

#include <exception>

#include "engine/core/Log.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

namespace gdl::game {
namespace {
constexpr f32 kReferenceHeight = 540.0f;
constexpr f32 kTextHeight = 12.0f;
constexpr f32 kMargin = 1.0f;
constexpr Color kTint = Color::rgba(255, 255, 255, 191);
} // namespace

BuildLabel::BuildLabel(std::string_view version) : m_text(formatVersion(version)) {}

std::string BuildLabel::formatVersion(std::string_view version) {
    std::string text = "Build v";
    text += version;
    return text;
}

Mat4 BuildLabel::projection(Extent2D extent) {
    if (extent.isZero()) {
        return Mat4{1};
    }
    const f32 width =
        kReferenceHeight * static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
    // Use the actual viewport, not the game's letterboxed 4:3 frame.
    return makeScreenProjection(width, kReferenceHeight);
}

bool BuildLabel::load(RenderDevice& device) {
    release();
    try {
        // Rasterize once, large enough for high-DPI displays. Never upload while drawing.
        const Image image = rasterizeSystemText(m_text, 48);
        m_texture = device.createTexture(
            {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            image.pixels);
    } catch (const std::exception& error) {
        release();
        log::warn("Build label: {}", error.what());
        return false;
    }
    return ready();
}

void BuildLabel::release() {
    m_texture.reset();
}

void BuildLabel::render(RenderDevice& device, Extent2D extent) const {
    if (!ready() || extent.isZero()) {
        return;
    }
    Canvas canvas;
    canvas.begin(device, projection(extent), {.depthWrite = false, .depthTest = false});
    const f32 width =
        kTextHeight * static_cast<f32>(m_texture->width()) / static_cast<f32>(m_texture->height());
    canvas.draw(*m_texture, {kMargin, kMargin, width, kTextHeight}, kTint);
    canvas.end();
}

} // namespace gdl::game
