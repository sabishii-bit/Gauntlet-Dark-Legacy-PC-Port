#include <array>
#include <format>
#include <string>

#include "engine/core/Types.h"

#include "game/menu/OptionMenu.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/ShopLayout.h"

namespace gdl::game {
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
void AfterLevelScene::prompt(s32 x, s32 y, s32 size) {
    if (const auto* button = texture("BUTTON_X")) {
        m_canvas.draw(*button, Rect{static_cast<f32>(x), static_cast<f32>(y),
                                    static_cast<f32>(size), static_cast<f32>(size)});
    }
}
void AfterLevelScene::drawBackground(s32 player) {
    const s32 x = player * 128;
    image(std::format("S1_PLYR{}", player + 1), x, 0);
    image(std::format("S2_PLYR{}", player + 1), x, 256);
    image("S1_BORDER", x, 0, Color::rgba(128, 128, 128, 128));
    image("S2_BORDER", x, 256, Color::rgba(128, 128, 128, 128));
}
void AfterLevelScene::drawPile(usize pile, s32 x, f32 height) {
    constexpr std::array<std::string_view, 3> kPiles{"SHP_GOLD", "SHP_BONES", "SHP_EXP"};
    if (const auto* art = texture(kPiles.at(pile))) {
        // Reveal the top of the pile without squeezing a 256-pixel image into a tiny icon.
        m_canvas.draw(*art, Rect{static_cast<f32>(x), LevelTally::kBaseline - height, 128, height},
                      Rect{0, 0, 1, height / 256}, Color::white());
    }
}
void AfterLevelScene::drawTally(const ShopLane& lane, s32 x) {
    constexpr std::array<std::string_view, 3> kLabels{"shop.gold", "shop.kills", "shop.experience"};
    for (usize rank = 0; rank < lane.tally.order().size(); ++rank) {
        const usize pile = lane.tally.order()[rank];
        if (rank <= lane.tally.growingRank()) {
            drawPile(pile, x, lane.tally.height(pile));
        }
        line(x + 16, 32 + static_cast<s32>(pile) * 20,
             std::format("{}: {}", text(kLabels[pile]), lane.tally.results().totals[pile]), 0.5f,
             Color::white(), rank == lane.tally.growingRank());
    }
    if (lane.tally.finished()) {
        prompt(x + 16, 89, 20);
        line(x + 32, 92, text("shop.continue"), 0.5f, Color::white(), true);
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
    if (lane.statsReady()) {
        prompt(x + 16, 280, 16);
        line(x + 40, 280, text("shop.continue"), 0.5f, Color::white(), true);
    }
}
void AfterLevelScene::drawShop(const ShopLane& lane, s32 x) {
    if (lane.member.save.gold > 0) {
        drawPile(0, x, lane.goldHeight);
    }
    image("SHOP_SCROLL_1", x, 0);
    image("SHOP_SCROLL_2", x, 256);
    image(std::format("SHOP_TOP_{}", colorCode(lane.member.save.color)), x + 32, 0);
    const auto& items = m_session.catalog().items();
    const auto layout = ShopLayout::make(items, lane.cursor, m_font.height());
    const f32 scroll = m_scroll[static_cast<usize>(lane.member.player)];
    bool up = false;
    bool down = false;
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const s32 y = static_cast<s32>(static_cast<f32>(layout.rows[i]) + scroll);
        const bool selected = i == lane.cursor;
        const bool available =
            item.type == 0 ||
            shopEligibility(lane.member.save, lane.stats, item) == ShopResult::Bought ||
            ownsShopItem(lane.member.save, item);
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
        image(item.texture, x + 20, y, Color::white().withAlpha(alpha));
        if (item.price > 0) {
            const bool owned = ownsShopItem(lane.member.save, item);
            line(x + 58, y + (owned ? -6 : 12), std::format("{}{}", text("shop.buy"), item.price),
                 0.5f, label, selected);
            if (owned) {
                line(x + 58, y + 12, std::format("{}{}", text("shop.sell"), item.price * 3 / 4),
                     0.5f, label, selected);
            }
        }
        s32 textY = y + (item.texture.empty() ? 12 : 32);
        std::string_view description = item.description;
        const f32 scale = 0.5f * item.scale;
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
        image("MORE_UP", x + 32, 32);
    }
    if (down) {
        image("MORE_DOWN", x + 32, 280);
    }
}
void AfterLevelScene::drawLane(const ShopLane& lane) {
    const auto& save = lane.member.save;
    const s32 x = lane.member.player * 128;
    switch (lane.phase) {
    case ShopPhase::Tally: drawTally(lane, x); break;
    case ShopPhase::BeforeStats:
    case ShopPhase::AfterStats: drawStats(lane, x); break;
    case ShopPhase::Shopping: drawShop(lane, x); break;
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
    m_canvas.begin(device, makeVirtualScreenTransform(projection, 512, 384, width, height));
    m_canvas.fill(Rect{0, 0, 512, 384}, Color::black());
    for (s32 player = 0; player < 4; ++player) {
        drawBackground(player);
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
