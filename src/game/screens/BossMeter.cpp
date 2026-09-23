#include "game/screens/BossMeter.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

constexpr std::string_view kBackground = "METER_BG";
constexpr std::string_view kFill = "METER_FG";

const Texture* textureOf(TextureSet* textures, RenderDevice& device, std::string_view stem,
                         int piece) {
    if (textures == nullptr) {
        return nullptr;
    }
    const std::string name = std::format("{}{}", stem, piece + 1);
    const auto index = textures->find(name);
    if (!index.has_value()) {
        return nullptr;
    }
    try {
        return &textures->texture(device, *index);
    } catch (const std::exception& e) {
        log::warn("Boss meter: texture {}: {}", name, e.what());
        return nullptr;
    }
}

} // namespace

bool BossMeter::bind(const CritterMeter& meter, TextureSet* textures, int left) {
    clear();
    if (!meter.shown || meter.pieces <= 0) {
        return false;
    }
    m_textures = textures;
    m_pieces = std::min(meter.pieces, kMostPieces);
    m_left = left;
    m_leftInset = meter.leftInset;
    m_rightInset = meter.rightInset;
    m_backed = meter.backed;
    return true;
}

void BossMeter::clear() {
    m_textures = nullptr;
    m_pieces = 0;
    m_left = 0;
    m_leftInset = 0;
    m_rightInset = 0;
    m_backed = false;
    m_max = 1.0f;
    m_shown = 0.0f;
    m_fresh = true;
    m_alive = true;
    m_frozen = false;
}

void BossMeter::update(int ticks, float health, float maxHealth, bool alive, bool frozen) {
    if (!bound()) {
        return;
    }
    m_max = std::max(maxHealth, 1.0f);
    m_alive = alive;
    m_frozen = frozen;
    const float target = std::max(health, 0.0f);
    if (m_fresh) {
        m_fresh = false; // first sight: as it is
        m_shown = target;
        return;
    }
    const float step = kEasePerTick * static_cast<float>(std::max(ticks, 0));
    if (m_shown > target) {
        m_shown = std::max(m_shown - step, target);
    } else if (m_shown < target) {
        m_shown = std::min(m_shown + step, target);
    }
}

/** The original's arithmetic: two strips share the health, the first's fill running from
 * its cap to its end over the first half, the second's from its start to its tail over the
 * rest; one strip runs the whole width less both. */
std::array<int, BossMeter::kMostPieces> BossMeter::fillWidths() const {
    std::array<int, kMostPieces> widths{};
    if (!bound()) {
        return widths;
    }
    const float fraction = std::clamp(m_shown / m_max, 0.0f, 1.0f);
    if (m_pieces >= 2) {
        const float share = 2.0f * fraction;
        widths[0] = share >= 1.0f
                        ? kPieceWidth
                        : static_cast<int>(share * static_cast<float>(kPieceWidth - m_leftInset) +
                                           static_cast<float>(m_leftInset));
        widths[1] = static_cast<int>(std::max(share - 1.0f, 0.0f) *
                                     static_cast<float>(kPieceWidth - m_rightInset));
    } else {
        widths[0] = static_cast<int>(fraction *
                                     static_cast<float>(kPieceWidth - m_leftInset - m_rightInset));
    }
    return widths;
}

void BossMeter::draw(Canvas& canvas, RenderDevice& device) const {
    if (!showing()) {
        return;
    }
    const std::array<int, kMostPieces> widths = fillWidths();
    const Color plain = Color::rgba(255, 255, 255, kAlpha);
    for (int piece = 0; piece < m_pieces; ++piece) {
        const auto x = static_cast<float>(m_left + piece * kPieceWidth);
        if (m_backed) {
            if (const Texture* back = textureOf(m_textures, device, kBackground, piece)) {
                canvas.draw(*back,
                            Rect{x, static_cast<float>(kY), static_cast<float>(kPieceWidth),
                                 static_cast<float>(back->height())},
                            m_frozen ? kFrozenTint : plain);
            }
        }
        const int width = widths[static_cast<std::size_t>(piece)];
        if (width <= 0) {
            continue;
        }
        if (const Texture* fill = textureOf(m_textures, device, kFill, piece)) {
            // Cropped, not squeezed: the fill's left `width` pixels.
            const float across = static_cast<float>(width) / static_cast<float>(kPieceWidth);
            canvas.draw(*fill,
                        Rect{x, static_cast<float>(kY), static_cast<float>(width),
                             static_cast<float>(fill->height())},
                        Rect{0.0f, 0.0f, across, 1.0f}, plain);
        }
    }
}

} // namespace gdl::game
