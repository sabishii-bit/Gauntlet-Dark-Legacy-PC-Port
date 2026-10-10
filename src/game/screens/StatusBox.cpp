#include "game/screens/StatusBox.h"

#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/PowerupSelector.h"

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
constexpr s32 kCountIconY = 264;
constexpr s32 kCountIconSize = 16;
constexpr s32 kCountTextX = 48;
constexpr s32 kCountTextY = 268;
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

void StatusBoxPainter::preloadStatus() {
    for (const auto* name : {"S3", "BK_RUNE_STONE_02", "S4", "S4_FRAME", "COIN", "HEART",
                             "KEY_ICON", "TRBO_FULL_NEW", "TRBO_GLINT", "TURBO_GLOW_NEW"}) {
        staticTexture(name);
    }
    for (const auto name : kPotionIcons) {
        staticTexture(name);
    }
    for (s32 character = 0; character < kClassCount; ++character) {
        selectTexture(std::format("S4_{}", classCode(character)));
    }
    for (s32 frame = 1; frame <= TurboMeter::kGleamFrames; ++frame) {
        staticTexture(std::format("TRBO_GLEEM{}", frame));
    }
    for (const auto name : kRelicColours) {
        staticTexture(std::format("SM_KEY_{}", name));
        for (s32 rune = 1; rune <= kRunesInColour; ++rune) {
            staticTexture(std::format("SM_RUNE_{}_{:02}", name, rune));
        }
    }
}

void StatusBoxPainter::release() {
    m_controlLabels = {};
    m_countTextures = nullptr;
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
        // init_frame_blits replaces S3 with the horned rune strip for a selected
        // character. It stays untinted; the class panel below carries player_rgb.
        if (const Texture* strip = staticTexture(view.active ? "BK_RUNE_STONE_02" : kStrip)) {
            canvas.draw(*strip,
                        Rect{static_cast<f32>(left), static_cast<f32>(kBarY),
                             static_cast<f32>(kWidth), static_cast<f32>(kBarHeight)},
                        Color::white());
        }
    }
    const Texture* panel = nullptr;
    if (view.mode != StatusBoxView::Mode::Plain) {
        panel = selectTexture(std::format("S4_{}", classCode(view.classIndex)));
    }
    if (panel != nullptr) {
        canvas.draw(*panel, box, boxTint(color, view.active));
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
    if (view.towerPrompt) {
        // Keep the choices, without controller-button hints.
        if (m_smallCaps.ready()) {
            TextStyle style;
            style.scale = kInTowerScale;
            m_smallCaps.draw(canvas, left + kPromptTextX, kWaitTextY, text("hud.waitInTower"),
                             style);
            m_smallCaps.draw(canvas, left + kPromptTextX, kQuitTextY, text("hud.quitGame"), style);
        }
        return;
    }
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
    if (view.mode == StatusBoxView::Mode::Status) {
        drawRelics(canvas, slot, view.runes,
                   view.keysShown ? std::optional<u16>{view.bossKeys} : std::nullopt);
    }
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
        m_score.draw(canvas, left + kHealthRight - m_score.measure(health), kValueY, health, style);
    }
    // What is carried shows over the gold and the health: keys to the left, potions (the
    // colour of the next to be thrown) to the right, each with its count in the costume's
    // colour.
    const auto carried = [&](std::string_view name, s32 iconX, s32 count, s32 countX) {
        if (count <= 0) {
            return;
        }
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture, Rect{static_cast<f32>(left + iconX), static_cast<f32>(kCarriedY),
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
    if (view.mode == StatusBoxView::Mode::Status && view.powerup) {
        drawUsage(canvas, left, *view.powerup, tint);
    }
}

std::string StatusBoxPainter::usageAmount(const PowerupSlot& slot) {
    const auto remaining = PowerupSelector::remaining(slot);
    if (!remaining) {
        return {};
    }
    return PowerupSelector::charged(slot) ? std::format("{:.0f}", *remaining)
                                          : std::format("{:.1f}", *remaining);
}

void StatusBoxPainter::drawUsage(Canvas& canvas, s32 left, const PowerupSlot& powerup, Color tint) {
    const std::string amount = usageAmount(powerup);
    if (amount.empty() || !m_score.ready() || !m_smallCaps.ready()) {
        return;
    }
    constexpr s32 kDecimalWidth = 2;
    constexpr s32 kLabelGap = 3;
    constexpr s32 kGlyphEdgePadding = 1; // Scaled glyphs can extend past their integer advance.
    constexpr std::string_view kSeconds = "s";
    const bool seconds = !PowerupSelector::charged(powerup);
    const s32 suffixWidth = seconds ? m_smallCaps.measure(kSeconds, kUsageNameScale) : 0;
    const s32 amountWidth =
        m_score.measure(amount, kUsageScale) + (seconds ? kDecimalWidth : 0) + suffixWidth;
    const s32 amountX = left + kUsageRight - amountWidth - kGlyphEdgePadding;
    const std::string_view label = text(powerupTextId(powerup.kind, powerup.flags));
    const s32 labelRoom = amountX - left - kUsageLeft - kLabelGap;
    const s32 labelWidth = m_smallCaps.measure(label, kUsageNameScale);
    TextStyle caption;
    caption.color = tint;
    caption.scale = kUsageNameScale;
    if (labelWidth > labelRoom) {
        caption.scale *= static_cast<f32>(std::max(labelRoom, 0)) / static_cast<f32>(labelWidth);
    }
    m_smallCaps.draw(canvas, left + kUsageLeft, kUsageY, label, caption);
    TextStyle digits;
    digits.color = tint;
    digits.scale = kUsageScale;
    s32 x = amountX;
    for (const char character : amount) {
        if (character == '.') {
            // SCORE has only digits. Keep their native artwork and supply the decimal point.
            canvas.fill(Rect{static_cast<f32>(x),
                             static_cast<f32>(kUsageY + m_score.lineHeight(kUsageScale) - 1), 1, 1},
                        tint);
            x += kDecimalWidth;
        } else {
            x = m_score.draw(canvas, x, kUsageY, std::string_view{&character, 1}, digits);
        }
    }
    if (seconds) {
        caption.scale = kUsageNameScale;
        m_smallCaps.draw(canvas, x, kUsageY, kSeconds, caption);
    }
}

void StatusBoxPainter::drawRelics(Canvas& canvas, s32 slot, u16 runes, std::optional<u16> keys) {
    const s32 left = slot * kWidth;
    const auto place = [&](std::string_view name, s32 x, s32 y) {
        if (const Texture* texture = staticTexture(name)) {
            canvas.draw(*texture, Rect{static_cast<f32>(left + x), static_cast<f32>(y),
                                       static_cast<f32>(texture->width()),
                                       static_cast<f32>(texture->height())});
        }
    };
    for (s32 rune = 0; rune < kRuneCount; ++rune) {
        if ((runes & (1U << static_cast<u32>(rune))) == 0) {
            continue;
        }
        const s32 trio = rune / kRunesInColour;
        place(std::format("SM_RUNE_{}_{:02}", kRelicColours[static_cast<usize>(trio)],
                          rune % kRunesInColour + 1),
              kRuneX + rune * kRuneStep + trio, kRuneY);
    }
    if (!keys.has_value()) {
        return;
    }
    for (s32 key = 0; key < kKeyCount; ++key) {
        if ((*keys & (1U << static_cast<u32>(key))) != 0) {
            place(std::format("SM_KEY_{}",
                              kRelicColours[static_cast<usize>(key) % kRelicColours.size()]),
                  kKeyX + key * kKeyStep, kKeyY);
        }
    }
}

void StatusBoxPainter::drawTurbo(Canvas& canvas, s32 slot, const TurboMeterLook& look) {
    const auto left = static_cast<f32>(slot * kWidth);
    const auto top = static_cast<f32>(kTurboY);
    if (const Texture* bar = staticTexture("TRBO_FULL_NEW")) {
        const auto width = static_cast<f32>(bar->width());
        const auto height = static_cast<f32>(bar->height());
        canvas.draw(*bar, Rect{left, top, width, height}, look.back);
        // The front colour is the same sheet squeezed about the bar's middle.
        const f32 across = std::max(width * look.fill, 2.0f);
        canvas.draw(*bar, Rect{left + (width - across) * 0.5f, top, across, height}, look.front);
    }
    if (const Texture* glint = staticTexture("TRBO_GLINT")) {
        canvas.draw(*glint, Rect{left, top, static_cast<f32>(glint->width()),
                                 static_cast<f32>(glint->height())});
    }
    if (look.glow > 0) {
        if (const Texture* glow = staticTexture("TURBO_GLOW_NEW")) {
            canvas.draw(
                *glow,
                Rect{left, top, static_cast<f32>(glow->width()), static_cast<f32>(glow->height())},
                Color::white().withAlpha(look.glow));
        }
    }
    if (look.gleam >= 0) {
        if (const Texture* gleam = staticTexture(std::format("TRBO_GLEEM{}", look.gleam + 1))) {
            canvas.draw(*gleam,
                        Rect{left + static_cast<f32>(kGleamX), static_cast<f32>(kGleamY),
                             static_cast<f32>(gleam->width()), static_cast<f32>(gleam->height())});
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
    if (const Texture* mark = countTexture(icon)) {
        canvas.draw(*mark,
                    Rect{static_cast<f32>(left + kCountIconX), static_cast<f32>(kCountIconY),
                         static_cast<f32>(kCountIconSize), static_cast<f32>(kCountIconSize)});
    }
    if (m_smallCaps.ready()) {
        TextStyle style;
        style.scale = kCountScale;
        m_smallCaps.draw(canvas, left + kCountTextX, kCountTextY,
                         std::format("{}/{}", count, total), style);
    }
}

const Texture* StatusBoxPainter::countTexture(std::string_view icon) {
    const Texture* mark = staticTexture(icon);
    if (mark == nullptr && m_countTextures != nullptr && m_device != nullptr) {
        if (const auto index = m_countTextures->find(icon)) {
            try {
                mark = &m_countTextures->texture(*m_device, *index);
            } catch (const std::exception& error) {
                log::warn("Pickup icon {}: {}", icon, error.what());
            }
        }
    }
    return mark;
}

void StatusBoxPainter::preloadPickups(std::span<const std::string_view> cards,
                                      std::span<const std::string_view> counts) {
    for (const auto card : cards) {
        staticTexture(card);
    }
    for (const auto count : counts) {
        countTexture(count);
    }
}

} // namespace gdl::game
