#include "game/menu/OptionMenu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

namespace {

constexpr int kGaramondFrames = 6;
constexpr int kGaramondDelay = 10;
constexpr int kBurnFrames = 5;
constexpr int kPlayerTagX = 106;
constexpr int kArrowGlyphX = 2;
constexpr int kArrowGlyphY = 12;
constexpr int kArrowGlyphWidth = 11;
constexpr int kArrowGlyphHeight = 10;
constexpr int kArrowSheetSize = 32;
constexpr int kArrowDrawScale = 2;
constexpr int kFullAlpha = 255;
constexpr int kDisabledAlpha = 128;

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, kFullAlpha));
}

/** Opacity after the original's font-alpha convention, where 0 is opaque and 255 invisible. */
std::uint8_t opacityFromFontAlpha(int fontAlpha) {
    return clampByte(kFullAlpha - fontAlpha);
}

/** How many lines a passage runs to. */
int lineCount(std::string_view passage) {
    if (passage.empty()) {
        return 0;
    }
    auto lines = static_cast<int>(std::ranges::count(passage, '\n'));
    if (passage.back() != '\n') {
        ++lines;
    }
    return lines;
}

/** One line of menu text in a fixed colour and glyph sheet. */
void drawLabel(Canvas& canvas, const TextPainter& painter, int x, int y, std::string_view text,
               float scale, Color color, const Texture* sheet) {
    TextStyle style;
    style.scale = scale;
    style.color = color;
    style.texture = sheet;
    painter.draw(canvas, x, y, text, style);
}

} // namespace

std::uint8_t pulseOpacity(int time, int radius, int hold) {
    const int span = radius * 2;
    int phase = time % (span + hold);
    if (phase > span) {
        phase = 0;
    } else if (phase > radius) {
        phase = span - phase;
    }
    const int ramp = (radius + phase * kFullAlpha - 1) / radius;
    return opacityFromFontAlpha(kDisabledAlpha - 1 - ramp / 2);
}

float OptionMenu::iconPixelsPerUnit(const MenuScreen& screen) {
    // The original projects the arrow through its camera: the focal length in pixels times the
    // model scale, over the arrow's distance.
    const float focal =
        (static_cast<float>(screen.width) / 2.0f) / std::tan(screen.horizontalFov / 2.0f);
    return kIconWorldScale * focal / kIconDepth;
}

void OptionMenu::open(const MenuDefinition& definition, const TextPainter& painter,
                      const MenuScreen& screen, int selection) {
    m_definition = definition;
    m_screen = screen;
    m_open = true;
    m_backdropReleased = false;
    m_time = 0;
    m_finishTimer = 0;
    m_iconY = 0;
    m_iconTimer = kIconGlideTicks;
    m_iconDrawY = 0;
    m_iconScale = iconPixelsPerUnit(screen);
    const auto count = static_cast<int>(m_definition.items.size());
    m_selection = count == 0 ? 0 : std::clamp(selection, 0, count - 1);
    if (count > 0 && !m_definition.items[static_cast<std::size_t>(m_selection)].enabled) {
        m_selection = nextEnabled(m_selection, 1);
    }

    m_lineHeight = painter.lineHeight(m_definition.scale);
    m_columnHeight = 0;
    m_columnWidth = 0;
    for (const MenuItem& item : m_definition.items) {
        m_columnHeight += m_lineHeight + item.extraSpacing;
        m_columnWidth = std::max(m_columnWidth, painter.measure(item.text, m_definition.scale));
    }

    if (m_definition.y == -1) {
        m_columnY = screen.height / 2 - m_columnHeight / 2;
    } else if (m_definition.y < 0) {
        m_columnY = -(m_definition.y + m_columnHeight / 2);
    } else {
        m_columnY = m_definition.y;
    }
    m_columnX = m_definition.x < 0 ? -(m_definition.x + m_columnWidth / 2) : m_definition.x;

    const int width = m_definition.backdropWidth < 0 ? m_columnWidth + kBackdropMargin * 2
                                                     : m_definition.backdropWidth;
    const int height = m_definition.backdropHeight < 0 ? m_columnHeight + kBackdropMargin * 2
                                                       : m_definition.backdropHeight;
    const int x =
        m_definition.backdropX < 0 ? screen.width / 2 - width / 2 : m_definition.backdropX;
    const int y =
        m_definition.backdropY < 0 ? screen.height / 2 - height / 2 : m_definition.backdropY;
    m_backdrop = Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                      static_cast<float>(height)};
    // A lone passage sits about the column's middle; several run down from where they start.
    m_bodyTop = m_definition.bodyY;
    if (m_definition.bodyY < 0) {
        int lines = 0;
        for (const std::string& passage : m_definition.body) {
            lines += lineCount(passage);
        }
        const int bodyHeight = lines * painter.lineHeight(m_definition.bodyScale);
        m_bodyTop = m_columnY + m_columnHeight / 2 - bodyHeight / 2;
    }
    glideIcon(0);
}

void OptionMenu::close() {
    if (!m_open) {
        return;
    }
    if (m_definition.fades) {
        m_finishTimer = std::max(m_finishTimer, 1);
    } else {
        m_open = false;
    }
}

MenuEvent OptionMenu::update(const MenuInput& input, int ticks) {
    if (!m_open) {
        return {};
    }
    if (m_finishTimer > 0) {
        m_finishTimer += ticks;
        if (m_finishTimer >= kFadeTicks) {
            m_open = false;
            m_finishTimer = 0;
            return MenuEvent{MenuAction::Closed, 0};
        }
        return {};
    }
    m_time += ticks;
    glideIcon(ticks);

    const auto count = static_cast<int>(m_definition.items.size());
    if (count == 0) {
        return input.back ? MenuEvent{MenuAction::Back, 0} : MenuEvent{};
    }
    if (input.select || (m_definition.startSelects && input.start)) {
        return MenuEvent{MenuAction::Choice,
                         m_definition.items[static_cast<std::size_t>(m_selection)].code};
    }
    if (input.back) {
        return MenuEvent{MenuAction::Back, 0};
    }
    if (input.down) {
        m_selection = nextEnabled(m_selection, 1);
        return MenuEvent{MenuAction::Moved, 0};
    }
    if (input.up) {
        m_selection = nextEnabled(m_selection, -1);
        return MenuEvent{MenuAction::Moved, 0};
    }
    return {};
}

/** The next enabled item from `from` in direction `step`, wrapping; `from` when none. */
int OptionMenu::nextEnabled(int from, int step) const {
    const auto count = static_cast<int>(m_definition.items.size());
    int index = from;
    for (int tries = 0; tries < count; ++tries) {
        index = (index + step + count) % count;
        if (m_definition.items[static_cast<std::size_t>(index)].enabled) {
            return index;
        }
    }
    return from;
}

int OptionMenu::itemY(std::size_t index) const {
    int y = m_columnY;
    for (std::size_t i = 0; i < index && i < m_definition.items.size(); ++i) {
        y += m_lineHeight + m_definition.items[i].extraSpacing;
    }
    return y;
}

std::uint8_t OptionMenu::fadeOpacity() const {
    if (!m_definition.fades) {
        return kFullAlpha;
    }
    int fade = 0;
    if (m_finishTimer > 0) {
        fade = m_finishTimer * kFullAlpha / kFadeTicks;
    } else if (m_time < kFadeTicks) {
        fade = (kFadeTicks - m_time) * kFullAlpha / kFadeTicks;
    }
    return opacityFromFontAlpha(fade);
}

float OptionMenu::iconAngle() const {
    float angle = (m_selection & 1) != 0 ? kPi : 0.0f;
    if (m_iconTimer > 0 && m_iconTimer < kIconGlideTicks) {
        angle += kPi * static_cast<float>(m_iconTimer) / static_cast<float>(kIconGlideTicks);
    }
    return angle;
}

void OptionMenu::glideIcon(int ticks) {
    if (m_definition.items.empty()) {
        return;
    }
    const int target = itemY(static_cast<std::size_t>(m_selection)) + m_lineHeight / 2;
    if (m_iconTimer == 0) {
        m_iconDrawY = m_iconY;
        if (target != m_iconY) {
            m_iconTimer = 1;
        }
    } else if (m_iconTimer < kIconGlideTicks) {
        m_iconTimer += ticks;
        m_iconDrawY = m_iconY + (target - m_iconY) * m_iconTimer / kIconGlideTicks;
    } else {
        m_iconY = target;
        m_iconDrawY = target;
        m_iconTimer = 0;
    }
}

const Texture* OptionMenu::itemSheet(const MenuTextures& textures, bool selected) const {
    if (m_definition.garamondIntro && textures.garamond[0] != nullptr) {
        const int frame = m_finishTimer > 0
                              ? kGaramondFrames - m_finishTimer * kGaramondFrames / kFadeTicks
                              : (m_time - kGaramondDelay) / 2;
        if (frame < 0) {
            return textures.garamond[0];
        }
        if (frame < kGaramondFrames) {
            const Texture* sheet = textures.garamond[static_cast<std::size_t>(frame)];
            return sheet != nullptr ? sheet : textures.garamond[0];
        }
    }
    if (!selected && m_definition.parchmentFont && textures.parchment != nullptr) {
        return textures.parchment;
    }
    return textures.font;
}

void OptionMenu::draw(Canvas& canvas, const TextPainter& painter,
                      const MenuTextures& textures) const {
    if (!m_open || !painter.ready()) {
        return;
    }
    const std::uint8_t fade = fadeOpacity();
    const Color white = Color::white().withAlpha(fade);
    const Texture* labelSheet = m_definition.parchmentFont && textures.parchment != nullptr
                                    ? textures.parchment
                                    : textures.font;

    const bool hasBackdrop = !m_definition.backdrop.empty() && textures.backdrop != nullptr;
    if (hasBackdrop && !m_backdropReleased) {
        canvas.draw(*textures.backdrop, m_backdrop,
                    m_definition.backdropFades ? white : Color::white());
        if (textures.burn[0] != nullptr) {
            const auto frame = static_cast<std::size_t>((m_time >> 3) % kBurnFrames);
            const Texture* burn =
                textures.burn[frame] != nullptr ? textures.burn[frame] : textures.burn[0];
            canvas.draw(*burn, m_definition.burnArea, white);
        }
    }
    if (hasBackdrop) {
        if (!m_definition.title.empty()) {
            const int x = -static_cast<int>(m_backdrop.x + m_backdrop.width / 2.0f);
            const int y = static_cast<int>(m_backdrop.y) + kTitleMargin;
            drawLabel(canvas, painter, x, y, m_definition.title, m_definition.titleScale, white,
                      labelSheet);
        }
        if (!m_definition.playerLabel.empty()) {
            const int y = static_cast<int>(m_backdrop.y) + kPlayerTagMargin;
            drawLabel(canvas, painter, -kPlayerTagX, y, m_definition.playerLabel, kPromptScale,
                      white, labelSheet);
        }
    }

    const bool garamondActive = m_definition.garamondIntro && textures.garamond[0] != nullptr &&
                                itemSheet(textures, false) != labelSheet;
    for (std::size_t i = 0; i < m_definition.items.size(); ++i) {
        const MenuItem& item = m_definition.items[i];
        const int y = itemY(i);
        const bool selected = static_cast<int>(i) == m_selection;
        if (selected) {
            const std::uint8_t pulse = pulseOpacity(m_time, kPulseTicks, kPulseHoldTicks);
            const auto glowAlpha = static_cast<std::uint8_t>(std::min<int>(fade, pulse));
            TextStyle glow;
            glow.scale = m_definition.scale;
            glow.color = m_definition.colors.hi.withAlpha(glowAlpha);
            glow.texture = textures.glow != nullptr ? textures.glow : textures.font;
            glow.expand = kGlowExpand;
            painter.draw(canvas, m_definition.x, y, item.text, glow);
            drawLabel(canvas, painter, m_definition.x, y, item.text, m_definition.scale,
                      m_definition.colors.on.withAlpha(fade), itemSheet(textures, true));
        } else {
            Color color = m_definition.parchmentFont && !garamondActive
                              ? white
                              : m_definition.colors.off.withAlpha(fade);
            if (!item.enabled) {
                color = color.withAlpha(static_cast<std::uint8_t>(fade / 2));
            }
            drawLabel(canvas, painter, m_definition.x, y, item.text, m_definition.scale, color,
                      itemSheet(textures, false));
        }
    }

    if (!m_backdropReleased && !m_definition.items.empty()) {
        if (textures.icon != nullptr && textures.icon->bound()) {
            textures.icon->draw(
                canvas,
                Vec2{static_cast<float>(m_columnX + kIconOffsetX), static_cast<float>(m_iconDrawY)},
                m_iconScale, iconAngle());
        } else if (textures.arrows != nullptr) {
            const auto width = static_cast<float>(kArrowGlyphWidth * kArrowDrawScale);
            const auto height = static_cast<float>(kArrowGlyphHeight * kArrowDrawScale);
            const auto centerX = static_cast<float>(m_columnX + kIconOffsetX);
            const Rect area{centerX - width / 2.0f, static_cast<float>(m_iconDrawY) - height / 2.0f,
                            width, height};
            const auto sheet = static_cast<float>(kArrowSheetSize);
            const Rect uv{static_cast<float>(kArrowGlyphX) / sheet,
                          static_cast<float>(kArrowGlyphY) / sheet,
                          static_cast<float>(kArrowGlyphWidth) / sheet,
                          static_cast<float>(kArrowGlyphHeight) / sheet};
            canvas.draw(*textures.arrows, area, uv, white);
        }
    }

    if (!m_definition.body.empty() && hasBackdrop) {
        const int lineHeight = painter.lineHeight(m_definition.bodyScale);
        const Color ink = m_definition.colors.off.withAlpha(fade);
        int y = m_bodyTop;
        for (const std::string& passage : m_definition.body) {
            std::size_t from = 0;
            while (from < passage.size()) {
                const std::size_t end = std::min(passage.find('\n', from), passage.size());
                drawLabel(canvas, painter, -(m_screen.width / 2), y,
                          std::string_view(passage).substr(from, end - from),
                          m_definition.bodyScale, ink, textures.font);
                y += lineHeight;
                from = end + 1;
            }
            y += m_definition.bodyGap;
        }
    }

    if (m_definition.prompts && m_finishTimer == 0) {
        // The prompts share the row evenly: one alone sits in the middle.
        const int count =
            (m_definition.backLabel.empty() ? 0 : 1) + (m_definition.selectLabel.empty() ? 0 : 1);
        const int slot = m_screen.width / (count + 1);
        int x = 0;
        for (const std::string* label : {&m_definition.backLabel, &m_definition.selectLabel}) {
            if (!label->empty()) {
                x += slot;
                drawLabel(canvas, painter, -x, m_definition.promptY, *label, kPromptScale, white,
                          labelSheet);
            }
        }
    }
}

} // namespace gdl::game
