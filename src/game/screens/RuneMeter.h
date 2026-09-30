#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

#include "game/players/Party.h"

namespace gdl::game {

/** init_thermometer / fn_80055678: the runestone finder earned by defeating temple
 * Skorne (or by beating the battlefield's third level while in that realm).
 * Decomp PlayerHasShard names runestones; PlayerHasRune names boss victory bits. */
class RuneMeter {
public:
    enum class Cue : u8 { None, Closer, Nearby };
    void begin(s32 rune, const Vec3& position, const Vec3& worldSize, s32 realm,
               std::span<const PartyMember> party);
    void clear();
    /** A missing/collected stone hides the finder for the rest of this visit. Cues are
     * consumed even when narration is busy, exactly as the original's two flags are. */
    Cue update(const Vec3& attention, bool available);
    bool visible() const { return m_visible; }
    f32 fill() const { return m_fill; }
    void draw(Canvas& canvas, const Texture& frame, const Texture& fillTexture) const;
    static bool eligible(s32 rune, s32 realm, std::span<const PartyMember> party);
    static f32 closeness(f32 distance, f32 worldDiagonal);
    struct Column {
        Rect area;
        Rect uv;
    };
    static Column column(f32 fill, f32 width);

private:
    Vec3 m_position{0};
    f32 m_diagonal = 0;
    f32 m_fill = 0;
    bool m_visible = false;
    bool m_closer = false;
    bool m_nearby = false;
};

} // namespace gdl::game
