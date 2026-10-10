#include "game/screens/RuneMeter.h"

#include <algorithm>
#include <cmath>

#include "game/world/LevelCatalog.h"

namespace gdl::game {

bool RuneMeter::eligible(s32 rune, s32 realm, std::span<const PartyMember> party) {
    if (rune < 0 || rune >= Relics::kRuneCount) {
        return false;
    }
    constexpr s32 kTemple = 5;
    constexpr s32 kBattlefield = 8;
    bool earned = false;
    for (const PartyMember& member : party) {
        const ClassProgress& progress = member.save.progress();
        // Any owner overrides another participant's permission to see the finder.
        if (progress.relics.hasRune(rune)) {
            return false;
        }
        earned |= realm == kBattlefield ? progress.levels.hasBeaten(kBattlefield, 2)
                                        : progress.relics.hasShard(LevelRef::orderOf(kTemple));
    }
    return earned;
}

void RuneMeter::begin(s32 rune, const Vec3& position, const Vec3& worldSize, s32 realm,
                      std::span<const PartyMember> party) {
    clear();
    m_position = position;
    m_diagonal = glm::length(worldSize);
    m_visible = eligible(rune, realm, party) && std::isfinite(m_diagonal) && m_diagonal > 0;
}

void RuneMeter::clear() {
    m_position = Vec3{0};
    m_diagonal = 0;
    m_fill = 0;
    m_visible = false;
    m_closer = false;
    m_nearby = false;
}

f32 RuneMeter::closeness(f32 distance, f32 worldDiagonal) {
    if (!std::isfinite(worldDiagonal) || worldDiagonal <= 0 || !std::isfinite(distance)) {
        return 0;
    }
    const f32 d = std::clamp((distance - 8.0f) / (0.7f * worldDiagonal), 0.0f, 1.0f);
    return (1.0f - d) * (1.0f - d);
}

RuneMeter::Cue RuneMeter::update(const Vec3& attention, bool available) {
    m_visible &= available;
    if (!m_visible) {
        return Cue::None;
    }
    m_fill = closeness(glm::distance(m_position, attention), m_diagonal);
    // Keep the else-if: entering both thresholds at once speaks on successive updates.
    if (!m_closer && m_fill > 0.75f) {
        m_closer = true;
        return Cue::Closer;
    }
    if (!m_nearby && m_fill > 0.975f) {
        m_nearby = true;
        return Cue::Nearby;
    }
    return Cue::None;
}

RuneMeter::Column RuneMeter::column(f32 fill, f32 width) {
    const f32 amount = std::clamp(fill, 0.0f, 1.0f);
    const f32 height = std::round(73.0f * amount);
    const f32 top = (101.0f - 73.0f * amount) / 128.0f;
    return {{392, 102 - height, width, height + 27}, {0, top, 1, 1 - top}};
}

void RuneMeter::draw(Canvas& canvas, const Texture& frame, const Texture& fillTexture) const {
    if (!m_visible) {
        return;
    }
    drawFill(canvas, frame, fillTexture, m_fill);
}

void RuneMeter::drawFill(Canvas& canvas, const Texture& frame, const Texture& fillTexture,
                         f32 fill) {
    canvas.draw(frame,
                {392, -1, static_cast<f32>(frame.width()), static_cast<f32>(frame.height())});
    const auto shape = column(fill, static_cast<f32>(fillTexture.width()));
    canvas.draw(fillTexture, shape.area, shape.uv, Color::white());
}

} // namespace gdl::game
