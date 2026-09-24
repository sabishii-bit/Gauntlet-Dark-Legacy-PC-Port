#include "game/menu/OptionMenu.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr s32 kGaramondFrames = 6;
constexpr s32 kGaramondDelay = 10;
constexpr s32 kBurnFrames = 5;
constexpr s32 kPlayerTagX = 106;
constexpr s32 kArrowGlyphX = 2;
constexpr s32 kArrowGlyphY = 12;
constexpr s32 kArrowGlyphWidth = 11;
constexpr s32 kArrowGlyphHeight = 10;
constexpr s32 kArrowSheetSize = 32;
constexpr s32 kArrowDrawScale = 2;
constexpr s32 kFullAlpha = 255;
constexpr s32 kDisabledAlpha = 128;

u8 clampByte(s32 value) {
    return static_cast<u8>(std::clamp(value, 0, kFullAlpha));
}

/** Opacity after the original's font-alpha convention, where 0 is opaque and 255 invisible. */
u8 opacityFromFontAlpha(s32 fontAlpha) {
    return clampByte(kFullAlpha - fontAlpha);
}

/** How many lines a passage runs to. */
s32 lineCount(std::string_view passage) {
    if (passage.empty()) {
        return 0;
    }
    auto lines = static_cast<s32>(std::ranges::count(passage, '\n'));
    if (passage.back() != '\n') {
        ++lines;
    }
    return lines;
}

/** One line of menu text in a fixed colour and glyph sheet. */
void drawLabel(Canvas& canvas, const TextPainter& painter, s32 x, s32 y, std::string_view text,
               f32 scale, Color color, const Texture* sheet) {
    TextStyle style;
    style.scale = scale;
    style.color = color;
    style.texture = sheet;
    painter.draw(canvas, x, y, text, style);
}

} // namespace

u8 pulseOpacity(s32 time, s32 radius, s32 hold) {
    const s32 span = radius * 2;
    s32 phase = time % (span + hold);
    if (phase > span) {
        phase = 0;
    } else if (phase > radius) {
        phase = span - phase;
    }
    const s32 ramp = (radius + phase * kFullAlpha - 1) / radius;
    return opacityFromFontAlpha(kDisabledAlpha - 1 - ramp / 2);
}

f32 OptionMenu::iconPixelsPerUnit(const MenuScreen& screen) {
    // The original projects the arrow through its camera: the focal length in pixels times the
    // model scale, over the arrow's distance.
    const f32 focal =
        (static_cast<f32>(screen.width) / 2.0f) / std::tan(screen.horizontalFov / 2.0f);
    return kIconWorldScale * focal / kIconDepth;
}

void OptionMenu::open(const MenuDefinition& definition, const TextPainter& painter,
                      const MenuScreen& screen, s32 selection) {
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
    const auto count = static_cast<s32>(m_definition.items.size());
    m_selection = count == 0 ? 0 : std::clamp(selection, 0, count - 1);
    if (count > 0 && !m_definition.items[static_cast<usize>(m_selection)].enabled) {
        m_selection = nextEnabled(m_selection, 1);
    }

    m_lineHeight = painter.lineHeight(m_definition.scale);
    m_columnHeight = 0;
    m_columnWidth = 0;
    for (const MenuItem& item : m_definition.items) {
        m_columnHeight += m_lineHeight + item.extraSpacing;
        const auto label =
            item.alternate.empty() ? item.text : item.text + "    " + item.alternate + " ~";
        m_columnWidth = std::max(m_columnWidth, painter.measure(label, m_definition.scale));
    }

    if (m_definition.y == -1) {
        m_columnY = screen.height / 2 - m_columnHeight / 2;
    } else if (m_definition.y < 0) {
        m_columnY = -(m_definition.y + m_columnHeight / 2);
    } else {
        m_columnY = m_definition.y;
    }
    m_columnX = m_definition.x < 0 ? -(m_definition.x + m_columnWidth / 2) : m_definition.x;

    const s32 width = m_definition.backdropWidth < 0 ? m_columnWidth + kBackdropMargin * 2
                                                     : m_definition.backdropWidth;
    const s32 height = m_definition.backdropHeight < 0 ? m_columnHeight + kBackdropMargin * 2
                                                       : m_definition.backdropHeight;
    const s32 x =
        m_definition.backdropX < 0 ? screen.width / 2 - width / 2 : m_definition.backdropX;
    const s32 y =
        m_definition.backdropY < 0 ? screen.height / 2 - height / 2 : m_definition.backdropY;
    m_backdrop = Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(width),
                      static_cast<f32>(height)};
    // A lone passage sits about the column's middle; several run down from where they start.
    m_bodyTop = m_definition.bodyY;
    if (m_definition.bodyY < 0) {
        s32 lines = 0;
        for (const std::string& passage : m_definition.body) {
            lines += lineCount(passage);
        }
        const s32 bodyHeight = lines * painter.lineHeight(m_definition.bodyScale);
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

MenuEvent OptionMenu::update(const MenuInput& input, s32 ticks) {
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

    const auto count = static_cast<s32>(m_definition.items.size());
    if (count == 0) {
        return input.back ? MenuEvent{MenuAction::Back, 0} : MenuEvent{};
    }
    if (input.select || (m_definition.startSelects && input.start)) {
        if (!m_definition.items[static_cast<usize>(m_selection)].enabled) {
            return {};
        }
        return MenuEvent{MenuAction::Choice,
                         m_definition.items[static_cast<usize>(m_selection)].code};
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
s32 OptionMenu::nextEnabled(s32 from, s32 step) const {
    const auto count = static_cast<s32>(m_definition.items.size());
    s32 index = from;
    for (s32 tries = 0; tries < count; ++tries) {
        index = (index + step + count) % count;
        if (m_definition.items[static_cast<usize>(index)].enabled) {
            return index;
        }
    }
    return from;
}

s32 OptionMenu::itemY(usize index) const {
    s32 y = m_columnY;
    for (usize i = 0; i < index && i < m_definition.items.size(); ++i) {
        y += m_lineHeight + m_definition.items[i].extraSpacing;
    }
    return y;
}

u8 OptionMenu::fadeOpacity() const {
    if (!m_definition.fades) {
        return kFullAlpha;
    }
    s32 fade = 0;
    if (m_finishTimer > 0) {
        fade = m_finishTimer * kFullAlpha / kFadeTicks;
    } else if (m_time < kFadeTicks) {
        fade = (kFadeTicks - m_time) * kFullAlpha / kFadeTicks;
    }
    return opacityFromFontAlpha(fade);
}

f32 OptionMenu::iconAngle() const {
    f32 angle = (m_selection & 1) != 0 ? kPi : 0.0f;
    if (m_iconTimer > 0 && m_iconTimer < kIconGlideTicks) {
        angle += kPi * static_cast<f32>(m_iconTimer) / static_cast<f32>(kIconGlideTicks);
    }
    return angle;
}

void OptionMenu::glideIcon(s32 ticks) {
    if (m_definition.items.empty()) {
        return;
    }
    const s32 target = itemY(static_cast<usize>(m_selection)) + m_lineHeight / 2;
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
        const s32 frame = m_finishTimer > 0
                              ? kGaramondFrames - m_finishTimer * kGaramondFrames / kFadeTicks
                              : (m_time - kGaramondDelay) / 2;
        if (frame < 0) {
            return textures.garamond[0];
        }
        if (frame < kGaramondFrames) {
            const Texture* sheet = textures.garamond[static_cast<usize>(frame)];
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
    const u8 fade = fadeOpacity();
    const Color white = Color::white().withAlpha(fade);
    const Texture* labelSheet = m_definition.parchmentFont && textures.parchment != nullptr
                                    ? textures.parchment
                                    : textures.font;

    const bool hasBackdrop = !m_definition.backdrop.empty() && textures.backdrop != nullptr;
    if (hasBackdrop && !m_backdropReleased) {
        canvas.draw(*textures.backdrop, m_backdrop,
                    m_definition.backdropFades ? white : Color::white());
        if (textures.burn[0] != nullptr) {
            const auto frame = static_cast<usize>((m_time >> 3) % kBurnFrames);
            const Texture* burn =
                textures.burn[frame] != nullptr ? textures.burn[frame] : textures.burn[0];
            canvas.draw(*burn, m_definition.burnArea, white);
        }
    }
    if (hasBackdrop) {
        if (!m_definition.title.empty()) {
            const s32 x = -static_cast<s32>(m_backdrop.x + m_backdrop.width / 2.0f);
            const s32 y = static_cast<s32>(m_backdrop.y) + kTitleMargin;
            drawLabel(canvas, painter, x, y, m_definition.title, m_definition.titleScale, white,
                      labelSheet);
        }
        if (!m_definition.playerLabel.empty()) {
            const s32 y = static_cast<s32>(m_backdrop.y) + kPlayerTagMargin;
            drawLabel(canvas, painter, -kPlayerTagX, y, m_definition.playerLabel, kPromptScale,
                      white, labelSheet);
        }
    }

    const bool garamondActive = m_definition.garamondIntro && textures.garamond[0] != nullptr &&
                                itemSheet(textures, false) != labelSheet;
    for (usize i = 0; i < m_definition.items.size(); ++i) {
        const MenuItem& item = m_definition.items[i];
        const s32 y = itemY(i);
        if (!item.alternate.empty()) {
            const auto first = item.text + "    ";
            const auto x = m_definition.x;
            const auto secondX = x + painter.measure(first, m_definition.scale);
            drawLabel(canvas, painter, x, y, first, m_definition.scale,
                      m_definition.colors.off.withAlpha(fade), textures.font);
            drawLabel(canvas, painter, secondX, y, item.alternate, m_definition.scale,
                      m_definition.colors.off.withAlpha(fade), textures.font);
            const bool alternate = item.markedPart == 2;
            const auto& activeText = alternate ? item.alternate : item.text;
            const s32 activeX = alternate ? secondX : x;
            if (static_cast<s32>(i) == m_selection) {
                TextStyle glow;
                glow.scale = m_definition.scale;
                glow.color = m_definition.colors.hi.withAlpha(
                    std::min(fade, pulseOpacity(m_time, kPulseTicks, kPulseHoldTicks)));
                glow.texture = textures.glow != nullptr ? textures.glow : textures.font;
                glow.expand = kGlowExpand;
                painter.draw(canvas, activeX, y, activeText, glow);
            }
            drawLabel(canvas, painter, activeX, y, activeText + " ~", m_definition.scale,
                      m_definition.colors.on.withAlpha(fade), itemSheet(textures, true));
            continue;
        }
        const auto label = item.text + (item.markedPart == 1 ? " ~" : "");
        const bool selected = static_cast<s32>(i) == m_selection;
        if (selected) {
            const u8 pulse = pulseOpacity(m_time, kPulseTicks, kPulseHoldTicks);
            const auto glowAlpha = static_cast<u8>(std::min<s32>(fade, pulse));
            TextStyle glow;
            glow.scale = m_definition.scale;
            glow.color = m_definition.colors.hi.withAlpha(glowAlpha);
            glow.texture = textures.glow != nullptr ? textures.glow : textures.font;
            glow.expand = kGlowExpand;
            painter.draw(canvas, m_definition.x, y, label, glow);
            drawLabel(canvas, painter, m_definition.x, y, label, m_definition.scale,
                      m_definition.colors.on.withAlpha(fade), itemSheet(textures, true));
        } else {
            Color color = m_definition.parchmentFont && !garamondActive
                              ? white
                              : m_definition.colors.off.withAlpha(fade);
            if (!item.enabled) {
                color = color.withAlpha(static_cast<u8>(fade / 2));
            }
            drawLabel(canvas, painter, m_definition.x, y, label, m_definition.scale, color,
                      itemSheet(textures, false));
        }
    }

    if (!m_backdropReleased && !m_definition.items.empty()) {
        if (textures.icon != nullptr && textures.icon->bound()) {
            textures.icon->draw(
                canvas,
                Vec2{static_cast<f32>(m_columnX + kIconOffsetX), static_cast<f32>(m_iconDrawY)},
                m_iconScale, iconAngle());
        } else if (textures.arrows != nullptr) {
            const auto width = static_cast<f32>(kArrowGlyphWidth * kArrowDrawScale);
            const auto height = static_cast<f32>(kArrowGlyphHeight * kArrowDrawScale);
            const auto centerX = static_cast<f32>(m_columnX + kIconOffsetX);
            const Rect area{centerX - width / 2.0f, static_cast<f32>(m_iconDrawY) - height / 2.0f,
                            width, height};
            const auto sheet = static_cast<f32>(kArrowSheetSize);
            const Rect uv{static_cast<f32>(kArrowGlyphX) / sheet,
                          static_cast<f32>(kArrowGlyphY) / sheet,
                          static_cast<f32>(kArrowGlyphWidth) / sheet,
                          static_cast<f32>(kArrowGlyphHeight) / sheet};
            canvas.draw(*textures.arrows, area, uv, white);
        }
    }

    if (!m_definition.body.empty() && hasBackdrop) {
        const s32 lineHeight = painter.lineHeight(m_definition.bodyScale);
        const Color ink = m_definition.colors.off.withAlpha(fade);
        s32 y = m_bodyTop;
        for (const std::string& passage : m_definition.body) {
            usize from = 0;
            while (from < passage.size()) {
                const usize end = std::min(passage.find('\n', from), passage.size());
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
        const s32 count =
            (m_definition.backLabel.empty() ? 0 : 1) + (m_definition.selectLabel.empty() ? 0 : 1);
        const s32 slot = m_screen.width / (count + 1);
        s32 x = 0;
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
