#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/TextPainter.h"

#include "game/hud/HudView.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/RuneMeter.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {
namespace {

using Layout = LegendsDreamcastHudLayout;

/** A selector's label is written in font 1 at its own size where it fits. */
constexpr f32 kLabelSize = 1.0f;

/** `value` kept within [0, most] in whole units, nought for no number: a point off the frame
 * stands a help scroll at the frame's edge all the same. */
s32 wholeWithin(f32 value, s32 most) {
    return std::isfinite(value)
               ? static_cast<s32>(std::lround(std::clamp(value, 0.0f, static_cast<f32>(most))))
               : 0;
}

/** Whether the legend items lead with the Dreamcast's own, place by place: realm n's in place
 * n - 1, each with its icon, and none of the others with one. */
constexpr bool ownItemsLead() {
    for (usize index = 0; index < Layout::kLegends.size(); ++index) {
        const bool own = index < Layout::kQuestPlaces.size();
        const LegendsDreamcastLegend& legend = Layout::kLegends.at(index);
        if (legend.icon.empty() == own || (own && legend.realm != static_cast<s32>(index) + 1)) {
            return false;
        }
    }
    return true;
}
static_assert(ownItemsLead());

/** Whether `legends` holds realm `realm`'s item. */
bool holds(u16 legends, s32 realm) {
    return (legends & (1U << static_cast<u32>(realm))) != 0;
}

} // namespace

s32 dreamcastSeatX(usize seat) {
    return Layout::kSeatX.at(seat);
}

s32 dreamcastSeatCentre(usize seat) {
    return Layout::kSeatCentre.at(seat);
}

std::array<std::optional<s32>, LegendsDreamcastHudLayout::kQuestPlaces.size()>
dreamcastQuestPlaces(u16 legends) {
    std::array<std::optional<s32>, Layout::kQuestPlaces.size()> places;
    for (usize place = 0; place < places.size(); ++place) {
        const s32 realm = Layout::kLegends.at(place).realm;
        if (holds(legends, realm)) {
            places.at(place) = realm;
        }
    }
    usize free = 0;
    for (usize index = places.size(); index < Layout::kLegends.size(); ++index) {
        const s32 realm = Layout::kLegends.at(index).realm;
        if (!holds(legends, realm)) {
            continue;
        }
        while (free < places.size() && places.at(free).has_value()) {
            ++free;
        }
        if (free == places.size()) {
            break;
        }
        places.at(free) = realm;
    }
    return places;
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

std::optional<LegendsDreamcastCard> dreamcastCard(std::string_view card) {
    for (const LegendsDreamcastCardKind& kind : Layout::kCards) {
        if (kind.card == card) {
            return kind.face.empty() ? LegendsDreamcastCard{.art = kind.card, .darkLegacy = true}
                                     : LegendsDreamcastCard{.art = kind.face, .darkLegacy = false};
        }
    }
    return std::nullopt;
}

bool dreamcastWaitHereShown(bool waiting, s32 joined, s64 step) {
    return waiting && joined >= Layout::kWaitLeastJoined && (step & Layout::kWaitBlink) != 0;
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

bool dreamcastPromptReturns(std::span<const HudSeat> seats, usize seat) {
    for (usize other = 0; other < seats.size(); ++other) {
        const HudSeat& shown = seats[other];
        const bool inGame = !shown.departed && shown.box.mode != StatusBoxView::Mode::Plain;
        const bool waitingAtExit = shown.turbo.has_value() && shown.waitingAtExit;
        if (other != seat && inGame && !waitingAtExit) {
            return false;
        }
    }
    return true;
}

LegendsDreamcastBoxKind dreamcastBoxKind(const HudSeat& seat) {
    const bool inGame = !seat.departed && seat.box.mode != StatusBoxView::Mode::Plain;
    return inGame && !seat.box.inTower ? LegendsDreamcastBoxKind::Panel
                                       : LegendsDreamcastBoxKind::Plain;
}

RuneMeter::Column dreamcastThermometerColumn(f32 fill, f32 width) {
    const f32 held = std::isfinite(fill) ? std::clamp(fill, 0.0f, 1.0f) : 0.0f;
    return RuneMeter::column(std::sqrt(held), width);
}

f32 dreamcastLabelScale(const TextPainter& font, std::string_view label) {
    // Font 1 at its own size, fitted as Dark Legacy's own label is.
    return PowerupSelector::labelScale(font, label, kLabelSize, Layout::kLabelWidest);
}

LegendsDreamcastHelpBox dreamcastHelpBox(std::span<const std::string> lines, Vec2 head,
                                         bool anchored, s32 seat) {
    usize longest = 0;
    for (const std::string& line : lines) {
        longest = std::max(longest, line.size());
    }
    const s32 width =
        (static_cast<s32>(longest) + Layout::kHelpMarginCharacters) * Layout::kHelpCharacterWidth;
    const s32 height =
        (static_cast<s32>(lines.size()) + Layout::kHelpMarginLines) * Layout::kHelpLineHeight;
    // A seat's message with nothing in the world to stand over stands over the seat's box.
    const bool overBox =
        !anchored && seat >= 0 && static_cast<usize>(seat) < Layout::kSeatCentre.size();
    const s32 anchorX = overBox ? dreamcastSeatCentre(static_cast<usize>(seat))
                                : wholeWithin(head.x, Layout::kFrameWidth);
    const s32 anchorY = overBox
                            ? Layout::kHelpFloor
                            : wholeWithin(head.y, Layout::kFrameHeight) - Layout::kHelpPointLift;
    // Kept on the frame as FUN_8c0e3420 keeps it: the top first, then the bottom; the left
    // first, then the right.
    s32 top = anchorY - Layout::kHelpOverAnchor - (height / 2);
    if (top < Layout::kHelpHighest) {
        top = Layout::kHelpHighest;
    } else if (top + height > Layout::kHelpFloor) {
        top = Layout::kHelpFloor - height;
    }
    s32 left = anchorX - (width / 2);
    if (left < 0) {
        left = 0;
    } else if (left + width > Layout::kFrameWidth) {
        left = Layout::kFrameWidth - width;
    }
    return {.x = left,
            .y = top,
            .width = width,
            .height = height,
            .textCentre = left + (width / 2),
            .textTop =
                top + ((height - (static_cast<s32>(lines.size()) * Layout::kHelpLineHeight)) / 2)};
}

usize dreamcastHelpScroll(s32 color) {
    const usize plain = Layout::kHelpScrolls.size() - 1;
    return color >= 0 && static_cast<usize>(color) < plain ? static_cast<usize>(color) : plain;
}

} // namespace gdl::game
