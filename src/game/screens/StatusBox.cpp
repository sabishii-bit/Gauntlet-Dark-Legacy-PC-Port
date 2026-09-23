#include "game/screens/StatusBox.h"

#include <cstddef>
#include <exception>
#include <format>

#include "engine/core/Log.h"

#include "game/players/ClassData.h"

namespace gdl::game {

namespace {

constexpr std::string_view kSelectDirectory = "SELECT";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kInitialsFile = "fonts/initials.json";
constexpr std::string_view kScoreFile = "fonts/score.json";
constexpr std::string_view kSmallCapsFile = "fonts/8hifonts.json";
constexpr int kInitialsSpaceWidth = 12;
constexpr int kScoreSpaceWidth = 9;
constexpr int kSmallCapsSpaceWidth = 8;
constexpr int kIconSize = 20;
constexpr int kIconY = 357;
constexpr int kCoinX = 6;
constexpr int kHeartX = 61;
constexpr int kGoldRight = 60;
constexpr int kHealthRight = 116;
constexpr int kValueY = 359;
constexpr int kNameY = 339;
constexpr int kLevelY = 326;
constexpr float kNameScale = 0.667f;
constexpr std::string_view kStrip = "S3";
constexpr int kCountIconX = 28;
constexpr int kCountIconY = 288;
constexpr int kCountIconSize = 16;
constexpr int kCountTextX = 48;
constexpr int kCountTextY = 292;
constexpr float kCountScale = 1.5f;

} // namespace

bool StatusBoxPainter::load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                            const StringTable* strings) {
    release();
    if (!m_select.load(unpackedRoot / kSelectDirectory) ||
        !m_static.load(unpackedRoot / kStaticDirectory) ||
        !m_fontInitials.load(unpackedRoot / kInitialsFile, kInitialsSpaceWidth)) {
        log::warn("Status boxes: unpacked panels or fonts not found under {}",
                  unpackedRoot.string());
        return false;
    }
    m_device = &device;
    m_strings = strings;
    const Texture* initials = staticTexture("INITIALS");
    if (initials == nullptr) {
        release();
        return false;
    }
    m_initials.setFont(&m_fontInitials, initials);
    // The number and caption fonts are optional: the boxes draw without them.
    if (m_fontScore.load(unpackedRoot / kScoreFile, kScoreSpaceWidth)) {
        m_score.setFont(&m_fontScore, staticTexture("SCORE"));
    }
    if (m_fontSmallCaps.load(unpackedRoot / kSmallCapsFile, kSmallCapsSpaceWidth)) {
        m_smallCaps.setFont(&m_fontSmallCaps, staticTexture("8HIFONTS"));
    }
    return true;
}

void StatusBoxPainter::release() {
    m_initials.setFont(nullptr, nullptr);
    m_score.setFont(nullptr, nullptr);
    m_smallCaps.setFont(nullptr, nullptr);
    m_select.releaseTextures();
    m_static.releaseTextures();
    m_device = nullptr;
    m_strings = nullptr;
}

std::string_view StatusBoxPainter::potionIcon(int kind) {
    return kind >= 0 && static_cast<std::size_t>(kind) < kPotionIcons.size()
               ? kPotionIcons[static_cast<std::size_t>(kind)]
               : kPotionIcons[0];
}

void StatusBoxPainter::draw(Canvas& canvas, int slot, const StatusBoxView& view, bool bar) {
    if (!loaded()) {
        return;
    }
    const int left = slot * kWidth;
    const Rect box{static_cast<float>(left), static_cast<float>(kY), static_cast<float>(kWidth),
                   static_cast<float>(kHeight)};
    const int color = view.active ? view.color : slot;
    if (bar) {
        if (const Texture* strip = staticTexture(kStrip)) {
            canvas.draw(*strip,
                        Rect{static_cast<float>(left), static_cast<float>(kBarY),
                             static_cast<float>(kWidth), static_cast<float>(kBarHeight)},
                        boxTint(color, view.active));
        }
    }
    const Texture* panel = nullptr;
    if (view.mode != StatusBoxView::Mode::Plain) {
        panel = selectTexture(std::format("S4_{}", classCode(view.classIndex)));
    }
    if (panel != nullptr) {
        canvas.draw(*panel, box);
    } else if (const Texture* stone = staticTexture("S4")) {
        canvas.draw(*stone, box, boxTint(color, view.active));
    }
    if (const Texture* frame = staticTexture("S4_FRAME")) {
        canvas.draw(*frame, box);
    }
    if (view.mode == StatusBoxView::Mode::Plain) {
        return;
    }
    if (view.turbo.has_value() && !view.inTower) {
        drawTurbo(canvas, slot, *view.turbo);
    }
    const Color tint = playerColor(color);
    if (view.inTower) {
        // The original hides all but the words.
        if (m_smallCaps.ready()) {
            TextStyle style;
            style.scale = kInTowerScale;
            style.color = tint;
            m_smallCaps.draw(canvas, -(left + kWidth / 2), kInTowerY, text("hud.inTower"), style);
        }
        return;
    }
    const auto icon = [&](std::string_view name, int x) {
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture,
                        Rect{static_cast<float>(left + x), static_cast<float>(kIconY),
                             static_cast<float>(kIconSize), static_cast<float>(kIconSize)});
        }
    };
    icon("COIN", kCoinX);
    icon("HEART", kHeartX);
    if (m_score.ready()) {
        TextStyle style;
        style.color = tint;
        const std::string gold = std::format("{}", view.gold);
        const std::string health = std::format("{}", view.health);
        m_score.draw(canvas, left + kGoldRight - m_score.measure(gold), kValueY, gold, style);
        m_score.draw(canvas, left + kHealthRight - m_score.measure(health), kValueY, health, style);
    }
    // What is carried shows over the gold and the health: keys to the left, potions (the
    // colour of the next to be thrown) to the right, each with its count in the costume's
    // colour.
    const auto carried = [&](std::string_view name, int iconX, int count, int countX) {
        if (count <= 0) {
            return;
        }
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture,
                        Rect{static_cast<float>(left + iconX), static_cast<float>(kCarriedY),
                             static_cast<float>(texture->width()),
                             static_cast<float>(texture->height())});
        }
        if (m_score.ready()) {
            TextStyle style;
            style.color = tint;
            style.scale = kCarriedScale;
            m_score.draw(canvas, left + countX, kCarriedTextY, std::format("{}", count), style);
        }
    };
    if (view.mode == StatusBoxView::Mode::Status) {
        carried("KEY_ICON", kKeyIconX, view.keys, kKeyCountX);
        carried(potionIcon(view.potionKind), kPotionIconX, view.potions, kPotionCountX);
    }
    const int centerX = -(left + kWidth / 2);
    if (view.mode == StatusBoxView::Mode::Status && m_smallCaps.ready()) {
        m_smallCaps.draw(canvas, centerX, kLevelY,
                         std::vformat(text("select.levelShort"), std::make_format_args(view.level)),
                         TextStyle{});
    }
    TextStyle nameStyle;
    nameStyle.scale = kNameScale;
    nameStyle.color = tint;
    m_initials.draw(canvas, centerX, kNameY, view.name, nameStyle);
}

void StatusBoxPainter::drawTurbo(Canvas& canvas, int slot, const TurboMeterLook& look) {
    const auto left = static_cast<float>(slot * kWidth);
    const auto top = static_cast<float>(kTurboY);
    if (const Texture* bar = staticTexture("TRBO_FULL_NEW")) {
        const auto width = static_cast<float>(bar->width());
        const auto height = static_cast<float>(bar->height());
        canvas.draw(*bar, Rect{left, top, width, height}, look.back);
        // The front colour is the same sheet squeezed about the bar's middle.
        const float across = std::max(width * look.fill, 2.0f);
        canvas.draw(*bar, Rect{left + (width - across) * 0.5f, top, across, height}, look.front);
    }
    if (const Texture* glint = staticTexture("TRBO_GLINT")) {
        canvas.draw(*glint, Rect{left, top, static_cast<float>(glint->width()),
                                 static_cast<float>(glint->height())});
    }
    if (look.glow > 0) {
        if (const Texture* glow = staticTexture("TURBO_GLOW_NEW")) {
            canvas.draw(*glow,
                        Rect{left, top, static_cast<float>(glow->width()),
                             static_cast<float>(glow->height())},
                        Color::white().withAlpha(look.glow));
        }
    }
    if (look.gleam >= 0) {
        if (const Texture* gleam = staticTexture(std::format("TRBO_GLEEM{}", look.gleam + 1))) {
            canvas.draw(*gleam,
                        Rect{left + static_cast<float>(kGleamX), static_cast<float>(kGleamY),
                             static_cast<float>(gleam->width()),
                             static_cast<float>(gleam->height())});
        }
    }
}

const Texture* StatusBoxPainter::selectTexture(std::string_view name) {
    const auto index = m_select.find(name);
    if (!index.has_value() || m_device == nullptr) {
        return nullptr;
    }
    try {
        return &m_select.texture(*m_device, *index);
    } catch (const std::exception& e) {
        log::warn("Status boxes: texture {}: {}", name, e.what());
        return nullptr;
    }
}

const Texture* StatusBoxPainter::staticTexture(std::string_view name) {
    const auto index = m_static.find(name);
    if (!index.has_value() || m_device == nullptr) {
        return nullptr;
    }
    try {
        return &m_static.texture(*m_device, *index);
    } catch (const std::exception& e) {
        log::warn("Status boxes: texture {}: {}", name, e.what());
        return nullptr;
    }
}

std::string_view StatusBoxPainter::text(std::string_view id) const {
    return m_strings != nullptr ? m_strings->get(id) : id;
}

void StatusBoxPainter::drawCard(Canvas& canvas, int slot, std::string_view card, int y) {
    if (!loaded()) {
        return;
    }
    const auto left = static_cast<float>(slot * kWidth);
    if (const Texture* strip = staticTexture(kStrip)) {
        canvas.draw(*strip, Rect{left, static_cast<float>(y), static_cast<float>(kWidth),
                                 static_cast<float>(kBarHeight)});
    }
    if (const Texture* face = staticTexture(card)) {
        canvas.draw(*face, Rect{left, static_cast<float>(y + kBarHeight),
                                static_cast<float>(kWidth), static_cast<float>(kHeight)});
    }
}

void StatusBoxPainter::drawCount(Canvas& canvas, int slot, std::string_view icon, int count,
                                 int total) {
    if (!loaded()) {
        return;
    }
    const int left = slot * kWidth;
    if (const Texture* mark = staticTexture(icon)) {
        canvas.draw(*mark,
                    Rect{static_cast<float>(left + kCountIconX), static_cast<float>(kCountIconY),
                         static_cast<float>(kCountIconSize), static_cast<float>(kCountIconSize)});
    }
    if (m_smallCaps.ready()) {
        TextStyle style;
        style.scale = kCountScale;
        m_smallCaps.draw(canvas, left + kCountTextX, kCountTextY,
                         std::format("{}/{}", count, total), style);
    }
}

} // namespace gdl::game
