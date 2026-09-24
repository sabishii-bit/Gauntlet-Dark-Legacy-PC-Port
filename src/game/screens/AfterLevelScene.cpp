#include "game/screens/AfterLevelScene.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <string>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
bool AfterLevelScene::open(RenderDevice& device, const GameContext& context,
                           std::span<const PartyMember> party,
                           std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
                           std::string_view levelName, bool towerShop) {
    close();
    m_context = context;
    m_device = &device;
    try {
        ShopCatalog catalog;
        ClassDataSet classes;
        if (!catalog.load(context.unpackedRoot / "shop/catalog.json") ||
            !classes.load(context.unpackedRoot / "pdata") ||
            !m_select.load(context.unpackedRoot / "SELECT") ||
            !m_inventory.load(context.unpackedRoot / "INVENTORY") ||
            !m_static.load(context.unpackedRoot / "STATIC") ||
            !m_font.load(context.unpackedRoot / "fonts/font8x8.json", 4)) {
            close();
            return false;
        }
        const auto font = m_static.find("FONT8X8");
        if (!font.has_value() || !m_boxes.load(device, context.unpackedRoot, context.strings)) {
            close();
            return false;
        }
        m_text.setFont(&m_font, &m_static.texture(device, *font));
        m_session.start(party, results, maxima, classes, std::move(catalog));
        if (towerShop) {
            m_session.skipTally();
        }
        // Decode every image before entering the screen: a broken export fails here,
        // not half-way through a transaction or render pass.
        for (const auto& item : m_session.catalog().items()) {
            if (!item.texture.empty() && texture(item.texture) == nullptr) {
                throw FormatError("shop: missing icon " + item.texture);
            }
        }
        texture("SHOP_SCROLL_1");
        texture("SHOP_SCROLL_2");
        for (const auto* name : {"SHP_GOLD", "SHP_BONES", "SHP_EXP"}) {
            texture(name);
        }
        for (const auto& lane : m_session.lanes()) {
            texture(std::format("SHOP_TOP_{}", colorCode(lane.member.save.color)));
        }
        m_open = true;
    } catch (const std::exception& e) {
        log::warn("After-level screen: {}", e.what());
        close();
        return false;
    }
    if (m_context.sounds != nullptr) {
        try {
            m_common.load(context.unpackedRoot / "audio/COMMON");
            const std::string bank =
                std::format("SHOP_{}", levelName.empty() ? 'G' : levelName.front());
            if (m_musicBank.load(context.unpackedRoot / "audio" / bank)) {
                if (const auto music = m_musicBank.find("S_" + bank); music.has_value()) {
                    m_music = m_context.sounds->play(m_musicBank.sequence(*music), 1,
                                                     SoundCategory::Music);
                }
            }
        } catch (const std::exception& e) {
            log::warn("Shop audio: {}", e.what());
        }
    }
    log::info("After-level tally/shop: {} players, {} catalog items", party.size(),
              m_session.catalog().items().size());
    return true;
}
void AfterLevelScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    m_text.setFont(nullptr, nullptr);
    m_boxes.release();
    m_select.releaseTextures();
    m_inventory.releaseTextures();
    m_static.releaseTextures();
    m_device = nullptr;
    m_open = false;
}
std::string_view AfterLevelScene::text(std::string_view id) const {
    return m_context.strings != nullptr ? m_context.strings->get(id) : id;
}
const Texture* AfterLevelScene::texture(std::string_view name) {
    const auto index = m_select.find(name);
    if (index.has_value()) {
        return &m_select.texture(*m_device, *index);
    }
    const auto inventory = m_inventory.find(name);
    return inventory.has_value() ? &m_inventory.texture(*m_device, *inventory) : nullptr;
}
void AfterLevelScene::sound(std::string_view name) {
    if (m_context.sounds == nullptr) {
        return;
    }
    if (const auto index = m_common.find(name); index.has_value()) {
        m_context.sounds->play(m_common.sequence(*index));
    }
}
bool AfterLevelScene::update(f64 seconds, const ShopSession::Inputs& inputs) {
    m_session.update(seconds, inputs);
    for (const auto& lane : m_session.lanes()) {
        const auto& input = inputs[static_cast<usize>(lane.member.player)];
        if (lane.transacted) {
            sound(lane.feedback == ShopResult::Bought || lane.feedback == ShopResult::Sold ||
                          lane.feedback == ShopResult::Exit
                      ? "S_OPTMENUSEL"
                      : "S_NO");
        } else if (lane.phase == ShopPhase::Shopping &&
                   (input.up || input.down || input.left || input.right)) {
            sound("S_OPTMENUMOVVRT");
        }
    }
    return m_session.finished();
}
void AfterLevelScene::line(s32 x, s32 y, std::string_view value, Color color) {
    m_text.draw(m_canvas, x, y, value, TextStyle{0.9f, color});
}
void AfterLevelScene::drawTally(const ShopLane& lane, s32 x) {
    constexpr std::array<std::string_view, 3> kLabels{"shop.gold", "shop.kills", "shop.experience"};
    constexpr std::array<std::string_view, 3> kIcons{"SHP_GOLD", "SHP_BONES", "SHP_EXP"};
    line(-(x + 64), 28, text("shop.results"));
    for (usize i = 0; i < kLabels.size(); ++i) {
        const s32 y = 57 + static_cast<s32>(i) * 64;
        if (const auto* icon = texture(kIcons[i])) {
            m_canvas.draw(*icon, Rect{static_cast<f32>(x + 7), static_cast<f32>(y), 22, 22});
        }
        line(x + 32, y, text(kLabels[i]), Color::rgba(255, 224, 140));
        const f32 fraction = lane.tally.fraction(i);
        const auto value =
            static_cast<s32>(static_cast<f64>(lane.tally.results().totals[i]) * fraction);
        line(x + 32, y + 13, std::to_string(value));
        m_canvas.fill(Rect{static_cast<f32>(x + 10), static_cast<f32>(y + 33), 108, 6},
                      Color::rgba(30, 24, 16));
        m_canvas.fill(Rect{static_cast<f32>(x + 10), static_cast<f32>(y + 33), 108 * fraction, 6},
                      playerColor(lane.member.save.color));
    }
    if (lane.tally.finished()) {
        line(-(x + 64), 278, text("shop.continue"));
    }
}
void AfterLevelScene::drawStats(const ShopLane& lane, s32 x) {
    const auto& save = lane.member.save;
    const auto level = experienceLevel(save.experience());
    const auto stats = save.character == kSumnerClass
                           ? masteryStats()
                           : displayStats(lane.stats, level, save.progress());
    constexpr std::array<std::string_view, 4> kLabels{"shop.strength", "shop.speed", "shop.armor",
                                                      "shop.magic"};
    line(-(x + 64), 28, text("shop.stats"));
    line(-(x + 64), 50, std::format("{} {}", text("shop.level"), level));
    for (usize i = 0; i < kLabels.size(); ++i) {
        const s32 y = 78 + static_cast<s32>(i) * 40;
        line(x + 10, y, text(kLabels[i]));
        line(x + 90, y, std::to_string(stats.values[i]), Color::rgba(255, 224, 140));
        m_canvas.fill(Rect{static_cast<f32>(x + 10), static_cast<f32>(y + 17),
                           108 * static_cast<f32>(stats.values[i]) / kMaxStat, 5},
                      playerColor(save.color));
    }
    line(-(x + 64), 248, text(lane.phase == ShopPhase::BeforeStats ? "shop.enter" : "shop.leave"));
    line(-(x + 64), 278, text("shop.continue"));
}
void AfterLevelScene::drawShop(const ShopLane& lane, s32 x) {
    const auto& items = m_session.catalog().items();
    line(-(x + 64), 28, text("shop.title"));
    const usize first =
        lane.cursor > 2 ? std::min(lane.cursor - 2, items.size() > 6 ? items.size() - 6 : 0) : 0;
    for (usize i = first; i < std::min(first + 6, items.size()); ++i) {
        const auto& item = items[i];
        const s32 y = 48 + static_cast<s32>(i - first) * 35;
        if (i == lane.cursor) {
            m_canvas.fill(Rect{static_cast<f32>(x + 5), static_cast<f32>(y - 2), 118, 34},
                          Color::rgba(80, 65, 18, 210));
        }
        if (const auto* icon = texture(item.texture)) {
            m_canvas.draw(*icon, Rect{static_cast<f32>(x + 8), static_cast<f32>(y + 3), 24, 24});
        }
        const auto eligibility = shopEligibility(lane.member.save, lane.stats, item);
        const Color color = eligibility == ShopResult::Bought || item.type == 0
                                ? Color::white()
                                : Color::rgba(150, 150, 150);
        std::string name = item.type == 0 ? std::string(text("shop.exit")) : item.description;
        std::ranges::replace(name, '\n', ' ');
        // Wrap on words in a narrow four-player lane; never silently truncate item names.
        s32 ty = y;
        while (!name.empty()) {
            usize length = name.size();
            while (length > 1 &&
                   m_text.measure(std::string_view(name).substr(0, length), 0.8f) > 84) {
                --length;
            }
            if (length < name.size()) {
                const usize space = name.rfind(' ', length);
                if (space != std::string::npos && space > 0) {
                    length = space;
                }
            }
            m_text.draw(m_canvas, x + 35, ty, std::string_view(name).substr(0, length),
                        TextStyle{0.8f, color});
            name.erase(0, length);
            while (!name.empty() && name.front() == ' ') {
                name.erase(0, 1);
            }
            ty += 8;
        }
        if (item.type != 0) {
            line(x + 35, y + 23,
                 std::format("{}{}", item.price, ownsShopItem(lane.member.save, item) ? " *" : ""),
                 Color::rgba(255, 224, 140));
        }
    }
    if (lane.feedbackLeft > 0) {
        std::string_view id = "shop.unavailable";
        switch (lane.feedback) {
        case ShopResult::Bought: id = "shop.bought"; break;
        case ShopResult::Sold: id = "shop.sold"; break;
        case ShopResult::InsufficientGold: id = "shop.no_gold"; break;
        case ShopResult::Full: id = "shop.full"; break;
        case ShopResult::NotOwned: id = "shop.not_owned"; break;
        default: break;
        }
        line(-(x + 64), 262, text(id), Color::rgba(255, 224, 140));
    }
    line(-(x + 64), 277, text("shop.buy_hint"));
    line(-(x + 64), 289, text("shop.sell_hint"));
}
void AfterLevelScene::drawLane(const ShopLane& lane) {
    const auto& save = lane.member.save;
    const s32 x = lane.member.player * 128;
    m_canvas.fill(Rect{static_cast<f32>(x + 2), 0, 124, 320}, Color::rgba(22, 18, 20));
    if (const auto* scroll = texture("SHOP_SCROLL_1")) {
        m_canvas.draw(*scroll, Rect{static_cast<f32>(x), 0, 128, 256});
    }
    if (const auto* scroll = texture("SHOP_SCROLL_2")) {
        m_canvas.draw(*scroll, Rect{static_cast<f32>(x), 256, 128, 64});
    }
    if (const auto* header = texture(std::format("SHOP_TOP_{}", colorCode(save.color)))) {
        m_canvas.draw(*header, Rect{static_cast<f32>(x + 32), 0, 64, 24});
    }
    // Keep the original scroll border, but give text contrast over its painted writing.
    m_canvas.fill(Rect{static_cast<f32>(x + 5), 25, 118, 274}, Color::rgba(12, 9, 6, 195));
    switch (lane.phase) {
    case ShopPhase::Tally: drawTally(lane, x); break;
    case ShopPhase::BeforeStats:
    case ShopPhase::AfterStats: drawStats(lane, x); break;
    case ShopPhase::Shopping: drawShop(lane, x); break;
    case ShopPhase::Done:
        line(-(x + 64), 150, text(lane.member.fallen ? "shop.fallen" : "shop.ready"));
        break;
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
    m_canvas.begin(device, makeVirtualScreenTransform(projection, 512, 384, width, height));
    m_canvas.fill(Rect{0, 0, 512, 384}, Color::black());
    for (const auto& lane : m_session.lanes()) {
        drawLane(lane);
    }
    m_canvas.end();
}
} // namespace gdl::game
