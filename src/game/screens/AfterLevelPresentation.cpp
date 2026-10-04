#include <algorithm>
#include <array>
#include <format>
#include <string>

#include "engine/core/Types.h"

#include "game/menu/OptionMenu.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/InventoryPanel.h"
#include "game/screens/ShopFrameLight.h"
#include "game/screens/ShopLayout.h"

namespace gdl::game {
namespace {
/** The magic perks' line of the level panel (shop_show_lv): MAGIC_ATT1 at level 25,
 * MAGIC_ATT2 at 50, the page by the class (char_type), left-aligned at the lane's middle
 * in 0xFF80C0. The unlockables' pages past the four are not there, so they see none. */
constexpr s32 kMagicLineY = 224;
constexpr s32 kMagicLineX = 64;
constexpr s32 kMagicPerkLevel = 25;
constexpr s32 kGreaterMagicPerkLevel = 50;
constexpr Color kMagicLineColor = Color::rgba(255, 128, 192);
constexpr Color kPriceFlash = Color::rgba(255, 0, 0);
constexpr s32 kWizardClass = 2;
constexpr Color kPointerHighlight = Color::rgba(130, 0, 234);
} // namespace
void AfterLevelScene::pointerTarget(const ShopLane& lane, PointerAction action, const Rect& area,
                                    usize row) {
    const auto left = static_cast<f32>(lane.member.player * StatusBoxPainter::kWidth);
    const f32 x = std::max(area.x, left);
    const f32 y = std::max(area.y, 0.0f);
    const f32 right = std::min(area.right(), left + StatusBoxPainter::kWidth);
    const f32 bottom = std::min(area.bottom(), static_cast<f32>(StatusBoxPainter::kY));
    if (right > x && bottom > y) {
        m_pointerTargets.push_back(
            {lane.member.player, lane.phase, action, row, Rect{x, y, right - x, bottom - y}});
    }
}
bool AfterLevelScene::pointerHovered(const ShopLane& lane, PointerAction action, usize row) const {
    const auto& hover = m_hoverTargets[static_cast<usize>(lane.member.player)];
    return hover && hover->phase == lane.phase && hover->action == action && hover->row == row;
}
void AfterLevelScene::drawContinue(const ShopLane& lane, s32 x, s32 y, s32 size, s32 labelX,
                                   s32 labelY) {
    constexpr f32 kScale = 0.5f;
    const auto label = text("shop.continue");
    prompt(lane.member.player, x, y, size);
    line(labelX, labelY, label, kScale,
         pointerHovered(lane, PointerAction::Continue) ? kPointerHighlight : Color::white(), true);
    const s32 right = std::max(x + size, labelX + m_text.measure(label, kScale));
    const s32 bottom = std::max(y + size, labelY + m_text.lineHeight(kScale));
    pointerTarget(lane, PointerAction::Continue,
                  Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(right - x),
                       static_cast<f32>(bottom - y)});
}
void AfterLevelScene::line(s32 x, s32 y, std::string_view value, f32 scale, Color color,
                           bool glow) {
    if (glow && m_glow != nullptr) {
        m_text.draw(m_canvas, x, y, value,
                    TextStyle{scale,
                              Color::rgba(130, 0, 234)
                                  .withAlpha(pulseOpacity(static_cast<s32>(m_time * 60), 40, 5)),
                              m_glow});
    }
    m_text.draw(m_canvas, x, y, value, TextStyle{scale, color});
}
void AfterLevelScene::image(std::string_view name, s32 x, s32 y, Color color) {
    if (const auto* art = texture(name)) {
        m_canvas.draw(*art,
                      Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(art->width()),
                           static_cast<f32>(art->height())},
                      color);
    }
}
void AfterLevelScene::prompt(s32 player, s32 x, s32 y, s32 size) {
    drawControlLabel(m_canvas, m_text,
                     Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(size),
                          static_cast<f32>(size)},
                     controlLabel(m_context.controlLabels, player, "menuSelect"));
}
void AfterLevelScene::drawBackground(s32 player) {
    const s32 x = player * 128;
    image(std::format("S1_PLYR{}", player + 1), x, 0);
    image(std::format("S2_PLYR{}", player + 1), x, 256);
}
void AfterLevelScene::drawFrame(std::string_view name, s32 x, s32 y) {
    const auto* art = texture(name);
    if (art == nullptr) {
        return;
    }
    ImmediateBatch batch;
    batch.begin(PrimitiveTopology::TriangleStrip);
    for (const Vec2& uv : {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}}) {
        const auto px = x + static_cast<s32>(uv.x * static_cast<f32>(art->width()));
        const auto py = y + static_cast<s32>(uv.y * static_cast<f32>(art->height()));
        const u8 light = shopFrameLight(px, 384 - py, m_frameTime);
        batch.vertex(Vec3{px, py, 0.5f}, Color::rgba(light, light, light, 255), uv);
    }
    batch.end();
    DrawState state;
    state.colorScale = 2;
    m_canvas.submit(batch, *art, Mat4{1}, state);
}
void AfterLevelScene::drawPile(usize pile, s32 x, f32 height) {
    constexpr std::array<std::string_view, 3> kPiles{"SHP_GOLD", "SHP_BONES", "SHP_EXP"};
    if (const auto* art = texture(kPiles.at(pile))) {
        // Reveal the top of the pile without squeezing a 256-pixel image into a tiny icon.
        m_canvas.draw(*art, Rect{static_cast<f32>(x), LevelTally::kBaseline - height, 128, height},
                      Rect{0, 0, 1, height / 256}, Color::white());
    }
}
void AfterLevelScene::drawLaneBackdrop(const ShopLane& lane) {
    const s32 x = lane.member.player * 128;
    if (lane.phase == ShopPhase::Tally) {
        // Growth follows height rank, but equal-depth sprites retain their creation
        // order: gold, bones, experience. Earnings must not change layering.
        const auto& order = lane.tally.order();
        for (usize pile = 0; pile < order.size(); ++pile) {
            const auto rank = static_cast<usize>(std::ranges::find(order, pile) - order.begin());
            if (rank <= lane.tally.growingRank()) {
                drawPile(pile, x, lane.tally.height(pile));
            }
        }
    } else if (lane.phase == ShopPhase::Shopping) {
        if (lane.member.save.gold > 0) {
            drawPile(0, x, lane.goldHeight);
        }
        image("SHOP_SCROLL_1", x, 0);
        image("SHOP_SCROLL_2", x, 256);
    }
}
void AfterLevelScene::drawTally(const ShopLane& lane, s32 x) {
    constexpr std::array<std::string_view, 3> kLabels{"shop.gold", "shop.kills", "shop.experience"};
    for (usize rank = 0; rank < lane.tally.order().size(); ++rank) {
        const usize pile = lane.tally.order()[rank];
        line(x + 16, 32 + static_cast<s32>(pile) * 20,
             std::format("{}: {}", text(kLabels[pile]), lane.tally.results().totals[pile]), 0.5f,
             Color::white(), rank == lane.tally.growingRank());
    }
    if (lane.tally.finished()) {
        drawContinue(lane, x + 16, 89, 20, x + 32, 92);
    }
    line(-(x + 64), 8, text("shop.stats"), 0.45f, Color::black());
}
std::string_view AfterLevelScene::rank(const ShopLane& lane) const {
    const auto& save = lane.member.save;
    const s32 level = experienceLevel(save.experience());
    std::string name;
    usize entry = 0;
    if (level == kMaxLevel) {
        name = "LEGEND";
    } else if (level < 10) {
        name = "PLAYER_CLASS_LC";
        entry = static_cast<usize>(save.character);
    } else {
        const auto code = classCode(save.character);
        name = std::format("{}_RANK", code == "DWF" ? "DWA" : code);
        entry = static_cast<usize>(level / 20);
    }
    if (const auto id = m_titles.find(name); id && entry < m_titles.message(*id).pages.size()) {
        return m_titles.message(*id).pages[entry];
    }
    return {};
}
void AfterLevelScene::drawStats(const ShopLane& lane, s32 x) {
    const bool promotion = lane.phase == ShopPhase::BeforeStats;
    line(-(x + 64), 8, text("shop.stats"), 0.45f, Color::black());
    line(-(x + 64), 32,
         std::format("{} {}", text("shop.level"), experienceLevel(lane.member.save.experience())),
         0.75f, Color::white(), promotion);
    line(-(x + 64), 64, rank(lane), 0.6f);
    constexpr std::array<std::string_view, 4> kLabels{"shop.strength", "shop.armor", "shop.magic",
                                                      "shop.speed"};
    const auto before = lane.statsValues(true);
    const auto after = lane.statsValues(false);
    const auto reveal = lane.statsRevealTicks();
    const f64 ticks = lane.phaseSeconds * 60;
    for (usize i = 0; i < kLabels.size(); ++i) {
        const bool changed = before[i] != after[i];
        const bool revealed = ticks > reveal[i];
        const s32 y = 96 + static_cast<s32>(i) * 20;
        line(x + 8, y, text(kLabels[i]), 0.48f, Color::white(),
             changed && revealed && ticks < reveal[i] + 60);
        line(x + 88, y, std::to_string(revealed ? after[i] : before[i]), 0.48f, Color::white(),
             changed && revealed);
    }
    const bool healthGlow = promotion && ticks > reveal[4];
    line(x + 8, 188, text("shop.max"), 0.48f, Color::white(), healthGlow && ticks < reveal[4] + 60);
    line(x + 8, 204, text("shop.health"), 0.48f, Color::white(),
         healthGlow && ticks < reveal[4] + 60);
    line(x + 88, 196, std::to_string(healthGlow ? after[4] : before[4]), 0.48f, Color::white(),
         healthGlow);
    if (promotion) {
        drawMagicLine(lane, x);
    }
    if (lane.statsReady()) {
        drawContinue(lane, x + 16, 280, 16, x + 40, 280);
    }
}
void AfterLevelScene::drawMagicLine(const ShopLane& lane, s32 x) {
    const auto& save = lane.member.save;
    const s32 level = experienceLevel(save.experience());
    if (level != kMagicPerkLevel && level != kGreaterMagicPerkLevel) {
        return;
    }
    const auto id = m_titles.find(level == kMagicPerkLevel ? "MAGIC_ATT1" : "MAGIC_ATT2");
    if (!id) {
        return;
    }
    const auto& message = m_titles.message(*id);
    const auto page = static_cast<usize>(
        save.character == kSumnerClass ? kWizardClass : save.character % kStartingClassCount);
    if (page >= message.pages.size()) {
        return;
    }
    std::string_view rest = message.pages[page];
    s32 y = kMagicLineY;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        line(x + kMagicLineX, y, rest.substr(0, end), message.scale, kMagicLineColor);
        if (end == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(end + 1);
        y += m_text.lineHeight(message.scale);
    }
}
void AfterLevelScene::drawShop(const ShopLane& lane, s32 x) {
    // do_shopping: 8034840C ("Shop"), scale 80348364 (0.45), font 6, black.
    line(-(x + 64), 8, text("shop.title"), 0.45f, Color::black());
    const auto& items = m_session.catalog().items();
    const auto layout = ShopLayout::make(items, lane.cursor, m_font.height());
    const f32 scroll = m_scroll[static_cast<usize>(lane.member.player)];
    bool up = false;
    bool down = false;
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const s32 y = static_cast<s32>(static_cast<f32>(layout.rows[i]) + scroll);
        const bool selected = i == lane.cursor;
        const bool available = lane.selectable(item);
        up |= available && y < ShopLayout::kTop;
        down |= available && y > ShopLayout::kBottom;
        u8 alpha = ShopLayout::opacity(static_cast<f32>(y));
        if (alpha == 0) {
            continue;
        }
        if (!available && alpha == 255) {
            alpha = 95;
        }
        const Color ink = Color::black().withAlpha(alpha);
        const Color label = selected ? Color::white() : ink;
        // A traded row's price shows red for thirty ticks once the cursor leaves it.
        const bool flashing = !selected && lane.flashTicks > 0 && i == lane.flashRow;
        const Color price = flashing ? kPriceFlash.withAlpha(alpha) : label;
        const bool interactive = available && alpha == 255;
        const bool owned = ownsShopItem(lane.member.save, item);
        const f32 scale = 0.5f * item.scale;
        const s32 descriptionY = y + (item.texture.empty() ? 12 : 32);
        const auto lines = 1 + static_cast<s32>(std::ranges::count(item.description, '\n'));
        const s32 rowTop = owned && item.price > 0 ? y - 6 : y;
        const s32 rowBottom = descriptionY + lines * m_text.lineHeight(scale);
        if (interactive) {
            pointerTarget(lane, PointerAction::Buy,
                          Rect{static_cast<f32>(x + 8), static_cast<f32>(rowTop), 112,
                               static_cast<f32>(rowBottom - rowTop)},
                          i);
        }
        image(item.texture, x + 20, y, Color::white().withAlpha(alpha));
        if (item.price > 0) {
            const auto buy = std::format("{}{}", text("shop.buy"), item.price);
            const s32 buyY = y + (owned ? -6 : 12);
            const bool hoveringSell = pointerHovered(lane, PointerAction::Sell, i);
            const bool hoveringBuy = pointerHovered(lane, PointerAction::Buy, i);
            line(x + 58, buyY, buy, 0.5f, price, selected && !hoveringSell);
            if (owned) {
                const auto sell = std::format("{}{}", text("shop.sell"), item.price * 3 / 4);
                line(x + 58, y + 12, sell, 0.5f, price, selected && !hoveringBuy);
                if (interactive) {
                    pointerTarget(lane, PointerAction::Sell,
                                  Rect{static_cast<f32>(x + 58), static_cast<f32>(y + 12),
                                       static_cast<f32>(m_text.measure(sell, 0.5f)),
                                       static_cast<f32>(m_text.lineHeight(0.5f))},
                                  i);
                }
            }
        }
        s32 textY = descriptionY;
        std::string_view description = item.description;
        while (!description.empty()) {
            const auto end = description.find('\n');
            line(-(x + 64), textY, description.substr(0, end), scale, label, selected);
            if (end == std::string_view::npos) {
                break;
            }
            description.remove_prefix(end + 1);
            textY += m_text.lineHeight(scale);
        }
    }
    if (up) {
        image("MORE_UP", x + 32, 32,
              pointerHovered(lane, PointerAction::Previous) ? kPointerHighlight : Color::white());
        if (const auto* art = texture("MORE_UP")) {
            pointerTarget(lane, PointerAction::Previous,
                          Rect{static_cast<f32>(x + 32), 32, static_cast<f32>(art->width()),
                               static_cast<f32>(art->height())});
        }
    }
    if (down) {
        image("MORE_DOWN", x + 32, 280,
              pointerHovered(lane, PointerAction::Next) ? kPointerHighlight : Color::white());
        if (const auto* art = texture("MORE_DOWN")) {
            pointerTarget(lane, PointerAction::Next,
                          Rect{static_cast<f32>(x + 32), 280, static_cast<f32>(art->width()),
                               static_cast<f32>(art->height())});
        }
    }
}
void AfterLevelScene::drawInventory(const ShopLane& lane, s32 x) {
    for (const InventoryPiece& piece : lane.inventory.pieces(x)) {
        const auto* art = texture(piece.texture);
        if (art == nullptr) {
            continue;
        }
        // The size is whole pixels, as the original projects its blits.
        const auto width = static_cast<s32>(static_cast<f32>(art->width()) * piece.size);
        const auto height = static_cast<s32>(static_cast<f32>(art->height()) * piece.size);
        m_canvas.draw(*art,
                      Rect{static_cast<f32>(piece.x), static_cast<f32>(piece.y),
                           static_cast<f32>(width), static_cast<f32>(height)},
                      Color::white().withAlpha(piece.opacity));
    }
    for (const InventoryCount& count : lane.inventory.counts(x)) {
        const TextStyle style{count.scale, Color::white().withAlpha(count.opacity)};
        const std::string have = std::to_string(count.n);
        m_text.draw(m_canvas, count.x - m_text.measure(have, count.scale), count.y, have, style);
        m_text.draw(m_canvas, count.x, count.y, std::format("/{}", count.m), style);
    }
    if (lane.inventory.showsPrompt()) {
        drawContinue(lane, x + InventoryPanel::kPromptX, InventoryPanel::kPromptY,
                     InventoryPanel::kPromptSize, x + InventoryPanel::kPromptLabelX,
                     InventoryPanel::kPromptY);
    }
}
void AfterLevelScene::drawLane(const ShopLane& lane) {
    const auto& save = lane.member.save;
    const s32 x = lane.member.player * 128;
    switch (lane.phase) {
    case ShopPhase::Tally: drawTally(lane, x); break;
    case ShopPhase::BeforeStats:
    case ShopPhase::AfterStats: drawStats(lane, x); break;
    case ShopPhase::FinalStats: drawFinalStats(lane, x); break;
    case ShopPhase::Shopping: drawShop(lane, x); break;
    case ShopPhase::Inventory: drawInventory(lane, x); break;
    case ShopPhase::Done: break;
    }
    StatusBoxView status;
    status.mode = StatusBoxView::Mode::Status;
    status.active = true;
    status.classIndex = save.character;
    status.color = save.color;
    status.name = save.name;
    status.level = experienceLevel(save.experience());
    status.gold = save.gold;
    status.health = save.health();
    status.keys = save.progress().inventory.keys;
    status.potions = static_cast<s32>(save.progress().inventory.potions.size());
    status.potionKind = save.progress().inventory.nextPotion();
    status.inTower = lane.member.fallen;
    m_boxes.draw(m_canvas, lane.member.player, status, false);
}
void AfterLevelScene::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    m_pointerTargets.clear();
    m_pointerTransform = makeVirtualScreenTransform(projection, 512, 384, width, height);
    m_canvas.begin(device, m_pointerTransform);
    m_canvas.fillScreen(Color::black());
    for (s32 player = 0; player < 4; ++player) {
        drawBackground(player);
    }
    // Retail depth: background 64000, piles 63990, parchment 63980, frames
    // 63900, colored caps 63800. Canvas uses submission order for these layers.
    for (const auto& lane : m_session.lanes()) {
        drawLaneBackdrop(lane);
    }
    for (s32 player = 0; player < 4; ++player) {
        // init_shop's 0x80808080 is neutral, not half-dark/half-transparent:
        // DrawBlit doubles/clamps alpha; the GX TEV stage doubles texture * RGB.
        // Keep the texture's own transparent cutouts, without another vertex fade.
        drawFrame("S1_BORDER", player * 128, 0);
        drawFrame("S2_BORDER", player * 128, 256);
        StatusBoxView empty;
        empty.color = player;
        m_boxes.draw(m_canvas, player, empty, false);
    }
    for (const auto& lane : m_session.lanes()) {
        // The colored cap belongs to the background, under the Stats lettering.
        image(std::format("SHOP_TOP_{}", colorCode(lane.member.save.color)),
              lane.member.player * 128 + 32, 0);
        drawLane(lane);
    }
    m_canvas.end();
}
} // namespace gdl::game
