#include "game/screens/InventoryPanel.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

#include "game/world/LevelTriggers.h"

namespace gdl::game {
namespace {
/** One SELECT picture of the panel and where its count is written (lbl_8011D568's rows:
 * the picture's place, then the count's scale and offset from it). */
struct PictureRow {
    std::string_view texture;
    s32 x;
    s32 y;
    f32 countScale;
    s32 countDx;
    s32 countDy;
};
constexpr f32 kPieceScale = 0.5f;
constexpr f32 kCrystalScale = 0.35f;
constexpr s32 kCountDy = 2;
/** The window, the three gargoyle pieces and the eight crystals, counted. */
constexpr std::array<PictureRow, 12> kPictures{{
    {"WINDOW_EMPTY", 0, 0, 0.0f, 0, 0},
    {"FANGS", 56, 116, kPieceScale, 30, kCountDy},
    {"FEATHER", 56, 140, kPieceScale, 30, kCountDy},
    {"CLAW", 56, 164, kPieceScale, 30, kCountDy},
    {"ORANGE_CRYSTLE", 6, 200, kCrystalScale, 28, kCountDy},
    {"RED_CRYSTLE", 6, 216, kCrystalScale, 28, kCountDy},
    {"PURPLE_CRYSTLE", 6, 232, kCrystalScale, 28, kCountDy},
    {"CYAN_CRYSTLE", 6, 248, kCrystalScale, 28, kCountDy},
    {"GREEN_CRYSTLE", 64, 200, kCrystalScale, 28, kCountDy},
    {"YELLOW_CRYSTLE", 64, 216, kCrystalScale, 28, kCountDy},
    {"WHITE_CRYSTLE", 64, 232, kCrystalScale, 28, kCountDy},
    {"BLACK_CRYSTLE", 64, 248, kCrystalScale, 28, kCountDy},
}};
constexpr usize kGargoyleFirst = 1;
constexpr usize kCrystalFirst = 4;

struct PieceRow {
    std::string_view texture;
    s32 x;
    s32 y;
};
/** The legend items by the realm of the boss each is for, with the picture of one not held. */
struct LegendRow {
    std::string_view texture;
    std::string_view empty;
    s32 x;
    s32 y;
};
constexpr std::array<LegendRow, 12> kLegends{{
    {"", "", 0, 0},
    {"SCIMITAR", "SCIMITAR_EMPTY", 96, 32},
    {"ICE_AX", "ICE_AX_EMPTY", 76, 32},
    {"LAMP", "LAMP_EMPTY", 94, 58},
    {"BILLOWS", "BILLOWS_EMPTY", 77, 56},
    {"SOUL_SAVIOR", "SOUL_SAVIOR_EMPTY", 98, 82},
    {"", "", 0, 0},
    {"BOOK", "BOOK_EMPTY", 54, 32},
    {"", "", 0, 0},
    {"FIRE_SCROLL", "FIRE_SCROLL_EMPTY", 56, 80},
    {"LANTERN", "LANTERN_EMPTY", 77, 81},
    {"JAVILIN", "JAVILIN_EMPTY", 54, 56},
}};
/** The boss glass pieces, shown for runestones 1 to 8 (PlayerHasRune, not the shards). */
constexpr std::array<PieceRow, 9> kGlass{{
    {"", 0, 0},
    {"LITCH_PIECE", 0, 86},
    {"DRAGON_PIECE", 0, 124},
    {"CHIMERA_PIECE", 0, 167},
    {"PLAGUE_PIECE", 22, 131},
    {"DRYDER_PIECE", 0, 106},
    {"GENIE_PIECE", 25, 72},
    {"YETTI_PIECE", 0, 148},
    {"WRAITH_PIECE", 0, 63},
}};

constexpr s32 kSpiralSteps = 60;  ///< the placement table: a turn in six-degree steps
constexpr f32 kSpiralRadius = 60; ///< pixels out the pieces start
constexpr f32 kSpiralTurns = 3;   ///< how far round the index runs over the entry
constexpr f32 kEnterSize = 2;     ///< the pieces start at twice their size
constexpr s32 kPushX = 64;        ///< the point the leaving pieces are pushed away from
constexpr s32 kPushY = 180;
constexpr f32 kPushRate = 10;
constexpr f32 kLeaveGrowth = 5;
constexpr f32 kFull = 255;

u8 opaque(s32 transparency) {
    return static_cast<u8>(
        std::clamp(static_cast<s32>(kFull) - transparency, 0, static_cast<s32>(kFull)));
}
} // namespace

InventoryContents InventoryContents::of(const ClassProgress& progress) {
    InventoryContents contents;
    contents.gargoylePieces = progress.relics.gargoylePieces;
    for (usize i = 0; i < kProvinceCount; ++i) {
        contents.crystals[i] = progress.crystals[i + 1];
    }
    contents.legends = progress.relics.legends;
    contents.runes = progress.relics.runes;
    return contents;
}

void InventoryPanel::open(const InventoryContents& contents) {
    m_contents = contents;
    m_phase = Phase::Entering;
    m_ticks = 0;
}

bool InventoryPanel::step(s32 ticks, bool accept) {
    if (m_phase == Phase::Closed || m_phase == Phase::Done) {
        return false;
    }
    m_ticks += std::max(ticks, 0);
    if (m_phase == Phase::Entering && m_ticks >= kEnterTicks) {
        m_phase = Phase::Idle;
    } else if (m_phase == Phase::Leaving && m_ticks >= kLeaveTicks) {
        m_phase = Phase::Done;
        return false;
    }
    if (accept && showsPrompt()) {
        m_phase = Phase::Leaving;
        m_ticks = 0;
        return true;
    }
    return false;
}

f32 InventoryPanel::progress() const {
    switch (m_phase) {
    case Phase::Entering: return static_cast<f32>(m_ticks) / static_cast<f32>(kEnterTicks);
    case Phase::Leaving: return static_cast<f32>(m_ticks) / static_cast<f32>(kLeaveTicks);
    default: return 0.0f;
    }
}

InventoryPanel::Placement InventoryPanel::place(s32 x, s32 y, s32 laneX) const {
    Placement placement{x + laneX, y};
    const f32 progress = this->progress();
    switch (m_phase) {
    case Phase::Entering: {
        const f32 squared = progress * progress;
        const s32 index =
            (y + x + static_cast<s32>(kSpiralRadius * (kSpiralTurns * (squared * progress)))) %
            kSpiralSteps;
        const f32 angle = static_cast<f32>(index) * 2.0f * std::numbers::pi_v<f32> /
                          static_cast<f32>(kSpiralSteps);
        const f32 factor = 1.0f - progress;
        placement.x = static_cast<s32>(static_cast<f32>(x + laneX) +
                                       factor * (kSpiralRadius * std::cos(angle)));
        placement.y =
            static_cast<s32>(static_cast<f32>(y) + factor * (kSpiralRadius * std::sin(angle)));
        placement.size = kEnterSize - squared;
        placement.opacity = opaque(static_cast<s32>(kFull * (1.0f - squared)));
        break;
    }
    case Phase::Leaving: {
        const f32 dx = static_cast<f32>(x - kPushX) * progress;
        const f32 dy = static_cast<f32>(y - kPushY) * progress;
        placement.x = static_cast<s32>(static_cast<f32>(x + laneX) + kPushRate * dx);
        placement.y = static_cast<s32>(static_cast<f32>(y) + kPushRate * dy);
        placement.size = 1.0f + kLeaveGrowth * progress;
        placement.opacity = opaque(static_cast<s32>(kFull * progress));
        break;
    }
    default: break;
    }
    return placement;
}

u8 InventoryPanel::textOpacity() const {
    switch (m_phase) {
    case Phase::Entering: return opaque(static_cast<s32>(kFull * (1.0f - progress())));
    case Phase::Leaving: return opaque(static_cast<s32>(kFull * progress()));
    default: return static_cast<u8>(kFull);
    }
}

std::vector<InventoryPiece> InventoryPanel::pieces(s32 laneX) const {
    std::vector<InventoryPiece> result;
    if (m_phase == Phase::Closed || m_phase == Phase::Done) {
        return result;
    }
    const auto add = [&](std::string_view texture, s32 x, s32 y) {
        const Placement placement = place(x, y, laneX);
        result.push_back({texture, placement.x, placement.y, placement.size, placement.opacity});
    };
    for (const PictureRow& row : kPictures) {
        add(row.texture, row.x, row.y);
    }
    for (usize realm = 0; realm < kLegends.size(); ++realm) {
        const LegendRow& row = kLegends[realm];
        if (row.texture.empty()) {
            continue;
        }
        const bool held = (m_contents.legends & (1U << realm)) != 0;
        add(held ? row.texture : row.empty, row.x, row.y);
    }
    for (usize rune = 0; rune < kGlass.size(); ++rune) {
        const PieceRow& row = kGlass[rune];
        if (!row.texture.empty() && (m_contents.runes & (1U << rune)) != 0) {
            add(row.texture, row.x, row.y);
        }
    }
    return result;
}

std::vector<InventoryCount> InventoryPanel::counts(s32 laneX) const {
    std::vector<InventoryCount> result;
    if (m_phase == Phase::Closed || m_phase == Phase::Done) {
        return result;
    }
    const u8 opacity = textOpacity();
    const auto add = [&](const PictureRow& row, s32 n, s32 m) {
        if (n < 0 || n > m) {
            n = m;
        }
        result.push_back(
            {laneX + row.x + row.countDx, row.y + row.countDy, row.countScale, n, m, opacity});
    };
    for (usize kind = 0; kind < Relics::kGargoyleKinds; ++kind) {
        add(kPictures[kGargoyleFirst + kind], m_contents.gargoylePieces[kind],
            Relics::kGargoyleNeeded[kind]);
    }
    for (usize province = 0; province < InventoryContents::kProvinceCount; ++province) {
        add(kPictures[kCrystalFirst + province], m_contents.crystals[province],
            LevelTriggers::kCrystalsToOpen[province + 1]);
    }
    return result;
}

} // namespace gdl::game
