#include "game/screens/StatusBox.h"

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
constexpr s32 kInitialsSpaceWidth = 12;
constexpr s32 kScoreSpaceWidth = 9;
constexpr s32 kSmallCapsSpaceWidth = 8;
constexpr s32 kIconSize = 20;
constexpr s32 kIconY = 357;
constexpr s32 kCoinX = 6;
constexpr s32 kHeartX = 61;
constexpr s32 kGoldRight = 60;
constexpr s32 kHealthRight = 116;
constexpr s32 kValueY = 359;
constexpr s32 kNameY = 339;
constexpr s32 kLevelY = 326;
constexpr f32 kNameScale = 0.667f;
constexpr std::string_view kStrip = "S3";
constexpr s32 kCountIconX = 28;
constexpr s32 kCountIconY = 288;
constexpr s32 kCountIconSize = 16;
constexpr s32 kCountTextX = 48;
constexpr s32 kCountTextY = 292;
constexpr f32 kCountScale = 1.5f;

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

std::string_view StatusBoxPainter::potionIcon(s32 kind) {
    return kind >= 0 && static_cast<usize>(kind) < kPotionIcons.size()
               ? kPotionIcons[static_cast<usize>(kind)]
               : kPotionIcons[0];
}

void StatusBoxPainter::draw(Canvas& canvas, s32 slot, const StatusBoxView& view, bool bar) {
    if (!loaded()) {
        return;
    }
    const s32 left = slot * kWidth;
    const Rect box{static_cast<f32>(left), static_cast<f32>(kY), static_cast<f32>(kWidth),
                   static_cast<f32>(kHeight)};
    const s32 color = view.active ? view.color : slot;
    if (bar) {
        if (const Texture* strip = staticTexture(kStrip)) {
            canvas.draw(*strip,
                        Rect{static_cast<f32>(left), static_cast<f32>(kBarY),
                             static_cast<f32>(kWidth), static_cast<f32>(kBarHeight)},
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
    const Color tint = playerColor(color);
    const auto icon = [&](std::string_view name, s32 x) {
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture, Rect{static_cast<f32>(left + x), static_cast<f32>(kIconY),
                                       static_cast<f32>(kIconSize), static_cast<f32>(kIconSize)});
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
        m_score.draw(canvas, left + kHealthRight - m_score.measure(health), kValueY, health,
                     style);
    }
    // What is carried shows over the gold and the health: keys to the left, potions (the
    // colour of the next to be thrown) to the right, each with its count in the costume's
    // colour.
    const auto carried = [&](std::string_view name, s32 iconX, s32 count, s32 countX) {
        if (count <= 0) {
            return;
        }
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture, Rect{static_cast<f32>(left + iconX),
                                       static_cast<f32>(kCarriedY),
                                       static_cast<f32>(texture->width()),
                                       static_cast<f32>(texture->height())});
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
    const s32 centerX = -(left + kWidth / 2);
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

void StatusBoxPainter::drawCard(Canvas& canvas, s32 slot, std::string_view card, s32 y) {
    if (!loaded()) {
        return;
    }
    const auto left = static_cast<f32>(slot * kWidth);
    if (const Texture* strip = staticTexture(kStrip)) {
        canvas.draw(*strip, Rect{left, static_cast<f32>(y), static_cast<f32>(kWidth),
                                 static_cast<f32>(kBarHeight)});
    }
    if (const Texture* face = staticTexture(card)) {
        canvas.draw(*face, Rect{left, static_cast<f32>(y + kBarHeight), static_cast<f32>(kWidth),
                                static_cast<f32>(kHeight)});
    }
}

void StatusBoxPainter::drawCount(Canvas& canvas, s32 slot, std::string_view icon, s32 count,
                                 s32 total) {
    if (!loaded()) {
        return;
    }
    const s32 left = slot * kWidth;
    if (const Texture* mark = staticTexture(icon)) {
        canvas.draw(*mark, Rect{static_cast<f32>(left + kCountIconX),
                                static_cast<f32>(kCountIconY), static_cast<f32>(kCountIconSize),
                                static_cast<f32>(kCountIconSize)});
    }
    if (m_smallCaps.ready()) {
        TextStyle style;
        style.scale = kCountScale;
        m_smallCaps.draw(canvas, left + kCountTextX, kCountTextY,
                         std::format("{}/{}", count, total), style);
    }
}

} // namespace gdl::game
