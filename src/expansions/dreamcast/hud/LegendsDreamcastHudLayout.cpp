#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include "engine/core/Types.h"

#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {
namespace {

using Layout = LegendsDreamcastHudLayout;

} // namespace

s32 dreamcastSeatX(usize seat) {
    return Layout::kSeatX.at(seat);
}

s32 dreamcastSeatCentre(usize seat) {
    return Layout::kSeatCentre.at(seat);
}

LegendsDreamcastTurboArch dreamcastTurbo(f32 shown, s32 seatX) {
    const f32 held = std::isfinite(shown) ? std::clamp(shown, 0.0f, Layout::kTurboMost) : 0.0f;
    const auto left = static_cast<f64>(seatX + Layout::kArtLeft);
    if (held >= Layout::kTurboFull) {
        return {.art = Layout::kTurboFullArt,
                .area = {static_cast<f32>(left), static_cast<f32>(Layout::kTurboTop),
                         Layout::kTurboWidth, Layout::kTurboRows},
                .uv = {0, 0, 1, 1},
                .full = true};
    }
    const LegendsDreamcastTurboZone* zone = &Layout::kTurboZones.back();
    for (const LegendsDreamcastTurboZone& candidate : Layout::kTurboZones) {
        if (held < candidate.from + candidate.length) {
            zone = &candidate;
            break;
        }
    }
    // Worked on the meter's own scale, so that a zone's marks fall where they are meant to.
    const f64 half =
        f64{zone->start} + f64{zone->span} * (f64{held} - f64{zone->from}) / f64{zone->length};
    // FUN_8c0c13e0 truncates the blit's left edge and its half-width (ftrc at 8c0c157c and
    // 8c0c154c) but samples the picture across the half-width as it is.
    const auto whole = static_cast<s32>(half);
    const s32 edge =
        static_cast<s32>(left + f64{Layout::kTurboCentre} - half) + Layout::kTurboLeftShift;
    return {.art = zone->art,
            .area = {static_cast<f32>(edge), static_cast<f32>(Layout::kTurboTop),
                     static_cast<f32>(2 * whole), Layout::kTurboRows},
            .uv = {static_cast<f32>((f64{Layout::kTurboCentre} - half) / f64{Layout::kTurboWidth}),
                   0, static_cast<f32>(2 * half / f64{Layout::kTurboWidth}), 1},
            .full = false};
}

u8 dreamcastTurboGlow(s64 steps) {
    const s64 within = std::max<s64>(steps, 0) % Layout::kGlowPeriod;
    s32 transparency = 0;
    for (s64 step = 0; step < within; ++step) {
        if (transparency % 2 == 0) {
            transparency += Layout::kGlowStep;
            if (transparency > Layout::kGlowMost) {
                transparency = Layout::kGlowTurn;
            }
        } else {
            transparency -= Layout::kGlowStep;
            if (transparency <= Layout::kGlowLeast) {
                transparency = 0;
            }
        }
    }
    return static_cast<u8>(transparency);
}

s32 dreamcastGoldLeft(s32 width, s32 seatX) {
    return width < Layout::kGoldNarrow ? seatX + Layout::kGoldRight - width
                                       : seatX + Layout::kGoldLeft;
}

std::optional<std::string_view> dreamcastPanelColour(s32 color) {
    if (color < 0 || static_cast<usize>(color) >= Layout::kPanelColours.size()) {
        return std::nullopt;
    }
    return Layout::kPanelColours.at(static_cast<usize>(color));
}

std::optional<std::string_view> dreamcastClassCode(s32 classIndex) {
    if (classIndex < 0 || static_cast<usize>(classIndex) >= Layout::kClassCodes.size()) {
        return std::nullopt;
    }
    return Layout::kClassCodes.at(static_cast<usize>(classIndex));
}

LegendsDreamcastBoxKind dreamcastBoxKind(const HudSeat& seat) {
    if (seat.departed && seat.runtime != nullptr) {
        return LegendsDreamcastBoxKind::Plain;
    }
    if (seat.box.mode == StatusBoxView::Mode::Plain) {
        return LegendsDreamcastBoxKind::None;
    }
    return seat.box.inTower ? LegendsDreamcastBoxKind::Plain : LegendsDreamcastBoxKind::Panel;
}

} // namespace gdl::game
