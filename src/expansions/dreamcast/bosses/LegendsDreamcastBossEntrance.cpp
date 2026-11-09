#include "expansions/dreamcast/bosses/LegendsDreamcastBossEntrance.h"

#include <array>
#include <cmath>

#include "engine/io/ByteReader.h"
#include "engine/math/Mt19937.h"

namespace gdl::game {
namespace {
bool finite(Vec3 point) {
    return !glm::any(glm::isnan(point)) && !glm::any(glm::isinf(point));
}
} // namespace

bool LegendsDreamcastBossEntrance::accepts(const formats::ItemInfoRecord& info,
                                           const formats::ItemInstanceRecord& instance) {
    const s16 override = static_cast<s16>(readU16LE(instance.params, 0));
    return info.type == 10 && (override > 0 ? override : info.subtype) == 51 &&
           finite(instance.position) && finite(instance.rotation);
}

void LegendsDreamcastBossEntrance::clear(f32 bottom) {
    *this = {};
    if (std::isfinite(bottom))
        m_bottom = bottom;
}

void LegendsDreamcastBossEntrance::add(u32 index, Vec3 position, Vec3 rotation) {
    if (m_phase != 0 || !finite(position) || !finite(rotation))
        return;
    Piece piece;
    piece.nativeIndex = index;
    piece.motion.place(position, rotation, index);
    m_pieces.push_back(piece);
}

void LegendsDreamcastBossEntrance::cue(Vec3 boss) {
    if (m_phase >= 3 || !finite(boss))
        return;
    ++m_phase;
    // SafeRockSetup 8c0a9f40 increments the E2 counter. do_items 8c0ad000
    // gives subtype 51 three successively stronger impulses; only the final
    // impulse sends the fragments away from Skorne horizontally.
    constexpr std::array<f32, 3> lifts{20, 30, 50};
    constexpr std::array<f32, 3> spreads{10, 15, 100};
    for (auto& piece : m_pieces) {
        auto& motion = piece.motion;
        if (!motion.visible)
            continue;
        if (m_phase == 3) {
            const Vec3 delta = motion.position - boss;
            const f32 length = std::hypot(delta.x, delta.z);
            motion.velocity = length > 0 ? Vec3{delta.x, 0, delta.z} * (10 / length) : Vec3{0};
        }
        const usize phase = static_cast<usize>(m_phase - 1);
        motion.velocity.y = lifts[phase] +
                            Mt19937::float24(static_cast<u32>(m_random())) * spreads[phase];
    }
}

void LegendsDreamcastBossEntrance::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0)
        return;
    m_updateSeconds = seconds;
    if (m_phase == 0)
        return;
    // The common loose-piece integrator is the same native 30 Hz gravity,
    // index-selected tumble and floor-sentinel discard as 8c0ad154..8c0ad2aa.
    for (s32 count = FallingPiece::framesDue(m_remainder, seconds); count > 0; --count)
        for (auto& piece : m_pieces)
            piece.motion.advance(FallingProfile{}, m_bottom);
}

f32 LegendsDreamcastBossEntrance::presentationAlpha(f32 alpha) const {
    return FallingPiece::presentationFraction(m_remainder, m_updateSeconds, alpha);
}

} // namespace gdl::game
