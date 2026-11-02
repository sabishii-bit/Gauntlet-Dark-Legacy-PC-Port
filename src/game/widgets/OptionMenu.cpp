#include "game/widgets/OptionMenu.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

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
constexpr f32 kArrowSize = 5;
constexpr f32 kArrowGap = 6;

bool contains(const Rect& area, const Vec2& point) {
    return point.x >= area.x && point.x < area.x + area.width && point.y >= area.y &&
           point.y < area.y + area.height;
}

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

MenuDefinition MenuDefinition::parchment() {
    MenuDefinition menu;
    menu.x = 128;
    menu.colors.off = Color::rgba(92, 26, 3);
    menu.parchmentFont = true;
    menu.backdrop = "SCROLL_A";
    menu.backdropY = 8;
    menu.backdropWidth = 480;
    menu.backdropHeight = 360;
    return menu;
}

MenuDefinition MenuDefinition::confirmation(std::string title, std::string no, std::string yes) {
    // The yes/no OPTMENU records: title scale 1.2, text x -256, SCROLL_A at (-1, 64) 320 by 220.
    constexpr f32 kTitleScale = 1.2f;
    constexpr s32 kTextX = -256;
    constexpr s32 kBackdropY = 64;
    constexpr s32 kBackdropWidth = 320;
    constexpr s32 kBackdropHeight = 220;
    MenuDefinition menu = parchment();
    menu.title = std::move(title);
    menu.titleScale = kTitleScale;
    menu.x = kTextX;
    menu.backdropX = -1;
    menu.backdropY = kBackdropY;
    menu.backdropWidth = kBackdropWidth;
    menu.backdropHeight = kBackdropHeight;
    menu.fades = true;
    menu.prompts = false;
    menu.fitWidth = kBackdropWidth; // nothing written on the dialog runs off its parchment
    menu.rowGap = OptionMenu::kDialogRowGap;
    menu.centreOnBackdrop = true; // the rows sit evenly under the title, or in the middle
    menu.items = {{std::move(no), 0}, {std::move(yes), 1}};
    return menu;
}

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
        (static_cast<f32>(screen.width) / 2.0f) / portable::tan(screen.horizontalFov / 2.0f);
    return kIconWorldScale * focal / kIconDepth;
}

void OptionMenu::open(const MenuDefinition& definition, const TextPainter& painter,
                      const MenuScreen& screen, s32 selection, s32 scroll) {
    m_definition = definition;
    m_painter = &painter;
    m_screen = screen;
    m_open = true;
    m_backdropReleased = false;
    // The pointer's place is kept: a page written again under a still pointer does not
    // choose the row under it until the pointer moves.
    m_hoverPart = 0;
    m_dragging = false;
    m_time = 0;
    m_finishTimer = 0;
    m_scroll = std::max(0, scroll);
    m_iconY = 0;
    m_iconTimer = kIconGlideTicks;
    m_iconDrawY = 0;
    m_iconRotation = 0;
    m_iconScale = iconPixelsPerUnit(screen) * m_definition.cursorScale;
    const auto count = static_cast<s32>(m_definition.items.size());
    m_definition.tailPositions.resize(
        std::min(m_definition.tailPositions.size(), m_definition.items.size()));
    m_selection = count == 0 ? 0 : std::clamp(selection, 0, count - 1);
    if (count > 0 && !m_definition.items[static_cast<usize>(m_selection)].enabled) {
        m_selection = nextEnabled(m_selection, 1);
    }

    // Rows and a title wider than the width they are given shrink to it, so a row added to
    // a dialog or a menu later never runs off its parchment or the screen.
    if (m_definition.fitWidth > 0) {
        const auto room = static_cast<f32>(std::max(1, m_definition.fitWidth - 2 * kFitMargin));
        s32 widest = 1;
        for (const MenuItem& item : m_definition.items) {
            const auto label =
                item.alternate.empty() ? item.text : item.text + "    " + item.alternate + " ~";
            widest = std::max(widest, painter.measure(label, 1.0f));
        }
        m_definition.scale = std::min(m_definition.scale, room / static_cast<f32>(widest));
        m_definition.cursorScale = std::min(m_definition.cursorScale, m_definition.scale);
        if (!m_definition.title.empty()) {
            const s32 titleWidth = std::max(1, painter.measure(m_definition.title, 1.0f));
            m_definition.titleScale =
                std::min(m_definition.titleScale, room / static_cast<f32>(titleWidth));
        }
    }
    m_lineHeight = painter.lineHeight(m_definition.scale);
    m_columnHeight = 0;
    m_columnWidth = 0;
    // The column is as tall as its window: the rows beyond it wait out of sight.
    const auto rows = static_cast<usize>(windowRows());
    for (usize i = 0; i < m_definition.items.size(); ++i) {
        const MenuItem& item = m_definition.items[i];
        if (i < rows) {
            m_columnHeight += m_lineHeight + item.extraSpacing;
            if (i + 1 < rows) {
                m_columnHeight += m_definition.rowGap;
            }
        }
        const auto label =
            item.alternate.empty() ? item.text : item.text + "    " + item.alternate + " ~";
        m_columnWidth = std::max(m_columnWidth, painter.measure(label, m_definition.scale));
    }

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
    // A title on the backdrop takes its cell at the title margin; nothing is written over it.
    const bool titled = !m_definition.title.empty() && !m_definition.backdrop.empty();
    m_titleBottom = titled ? y + kTitleMargin + painter.lineHeight(m_definition.titleScale) : 0;

    if (m_definition.y == -1) {
        s32 top = 0;
        s32 bottom = screen.height;
        if (m_definition.centreOnBackdrop && !m_definition.backdrop.empty()) {
            top = titled ? m_titleBottom : y;
            bottom = y + height;
        }
        m_columnY = (top + bottom) / 2 - m_columnHeight / 2;
    } else if (m_definition.y < 0) {
        m_columnY = -(m_definition.y + m_columnHeight / 2);
    } else {
        m_columnY = m_definition.y;
    }
    if (titled) {
        // A column under the title starts below it; rows placed by hand move down together.
        if (m_definition.itemPositions.empty()) {
            m_columnY = std::max(m_columnY, m_titleBottom);
        } else {
            f32 lowest = std::numeric_limits<f32>::max();
            for (const Vec2& at : m_definition.itemPositions) {
                lowest = std::min(lowest, at.y);
            }
            if (lowest < static_cast<f32>(m_titleBottom)) {
                const f32 down = static_cast<f32>(m_titleBottom) - lowest;
                for (Vec2& at : m_definition.itemPositions) {
                    at.y += down;
                }
            }
        }
    }
    m_columnX = m_definition.x < 0 ? -(m_definition.x + m_columnWidth / 2) : m_definition.x;
    ensureVisible();
    layoutAreas(painter);
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

void OptionMenu::layoutAreas(const TextPainter& painter) {
    m_itemAreas.clear();
    m_alternateX.clear();
    m_columnRight = 0;
    for (usize i = 0; i < m_definition.items.size(); ++i) {
        const auto& item = m_definition.items[i];
        const auto label = item.text + (item.markedPart == 1 ? " ~" : "");
        auto x = painter.leftEdge(itemX(i), label, m_definition.scale);
        auto right = x + painter.measure(label, m_definition.scale);
        const auto alternate = itemX(i) + painter.measure(item.text + "    ", m_definition.scale);
        if (!item.alternate.empty()) {
            right = alternate + painter.measure(item.alternate + " ~", m_definition.scale);
        }
        if (!item.value.empty()) {
            x = std::min(x, valueX(i) - static_cast<s32>(kArrowGap + kArrowSize));
            right = std::max(right,
                             valueX(i) + valueWidth(i) + static_cast<s32>(kArrowGap + kArrowSize));
        }
        // The column's right edge is its own rows' (the tail stands apart), in or out of view.
        if (static_cast<s32>(i) < columnCount()) {
            m_columnRight = std::max(m_columnRight, right);
        }
        // A row scrolled out of the window has nowhere to be pointed at.
        m_itemAreas.push_back(shown(i) ? Rect{static_cast<f32>(x), static_cast<f32>(itemY(i)),
                                              static_cast<f32>(right - x),
                                              static_cast<f32>(m_lineHeight)}
                                       : Rect{});
        m_alternateX.push_back(alternate);
    }
}

s32 OptionMenu::columnCount() const {
    return static_cast<s32>(m_definition.items.size() - m_definition.tailPositions.size());
}

s32 OptionMenu::windowRows() const {
    const s32 column = columnCount();
    return m_definition.visibleRows > 0 ? std::min(m_definition.visibleRows, column) : column;
}

bool OptionMenu::shown(usize item) const {
    if (item >= m_definition.items.size()) {
        return false;
    }
    const auto index = static_cast<s32>(item);
    if (index >= columnCount() || item < m_definition.itemPositions.size()) {
        return true;
    }
    return index >= m_scroll && index < m_scroll + windowRows();
}

void OptionMenu::ensureVisible() {
    const s32 column = columnCount();
    const s32 rows = windowRows();
    const s32 most = std::max(0, column - rows);
    m_scroll = std::clamp(m_scroll, 0, most);
    if (m_selection < column) {
        if (m_selection < m_scroll) {
            m_scroll = m_selection;
        } else if (m_selection >= m_scroll + rows) {
            m_scroll = m_selection - rows + 1;
        }
    }
    if (m_painter != nullptr) {
        layoutAreas(*m_painter);
    }
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

void OptionMenu::closeWithFade() {
    m_definition.fades = true;
    close();
}

std::optional<usize> OptionMenu::pointerChoice(const MenuInput& input) const {
    if (!m_open || closing() || input.pointerNormalized || !input.pointer ||
        !input.pointerPressed) {
        return std::nullopt;
    }
    for (usize i = 0; i < m_itemAreas.size(); ++i) {
        if (m_definition.items[i].enabled && contains(m_itemAreas[i], *input.pointer)) {
            return i;
        }
    }
    return std::nullopt;
}

MenuEvent OptionMenu::update(const MenuInput& input, s32 ticks, std::optional<usize> itemChoice) {
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
    const s32 previous = m_selection;
    if (itemChoice && *itemChoice < m_definition.items.size() &&
        m_definition.items[*itemChoice].enabled) {
        m_lastPointer.reset();
        m_hoverPart = 0;
        focus(*itemChoice);
        return {MenuAction::Choice, m_definition.items[*itemChoice].code};
    }
    // The bar at the column's right hand: pressed on its marks it steps a row, pressed on
    // its track the thumb follows the pointer for as long as it is held.
    if (!input.pointerNormalized && input.pointer && windowRows() < columnCount()) {
        if (input.pointerPressed && contains(scrollBarArea(), *input.pointer)) {
            const ScrollBar bar = scrollBar();
            if (input.pointer->y < bar.trackTop) {
                scrollWindow(m_scroll - 1);
            } else if (input.pointer->y >= bar.trackTop + bar.trackHeight) {
                scrollWindow(m_scroll + 1);
            } else {
                m_dragging = true;
            }
            m_lastPointer = input.pointer;
            return {};
        }
        if (m_dragging) {
            if (!input.pointerHeld) {
                m_dragging = false;
            } else {
                scrollWindow(scrollAt(input.pointer->y));
                m_lastPointer = input.pointer;
                return {};
            }
        }
    } else {
        m_dragging = false;
    }
    if (!input.pointerNormalized && input.pointer) {
        // A pointer chooses the row under it where it appears and once it has moved a
        // little, never while it only rests where the rows shift under it.
        const bool moved = !m_lastPointer.has_value() ||
                           glm::distance(*m_lastPointer, *input.pointer) > kHoverMotion;
        if (moved) {
            m_lastPointer = input.pointer;
        }
        if (moved || input.pointerPressed) {
            m_hoverPart = 0;
            for (usize i = 0; i < m_itemAreas.size(); ++i) {
                const auto& item = m_definition.items[i];
                if (!item.enabled || !contains(m_itemAreas[i], *input.pointer)) {
                    continue;
                }
                focus(i);
                if (!item.alternate.empty()) {
                    m_hoverPart = input.pointer->x >= static_cast<f32>(m_alternateX[i]) ? 2 : 1;
                }
                if (input.pointerPressed) {
                    const s32 direction = !item.value.empty() &&
                                                  input.pointer->x >= static_cast<f32>(valueX(i)) -
                                                                          kArrowGap - kArrowSize &&
                                                  input.pointer->x < static_cast<f32>(valueX(i))
                                              ? -1
                                              : 1;
                    return {MenuAction::Choice, item.code, m_hoverPart, direction};
                }
                break;
            }
        }
    } else {
        m_lastPointer.reset();
        m_hoverPart = 0;
    }
    if (input.up || input.down || input.left || input.right || input.select || input.start) {
        m_hoverPart = 0;
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
        ensureVisible();
        return MenuEvent{MenuAction::Moved, 0};
    }
    if (input.up) {
        m_selection = nextEnabled(m_selection, -1);
        ensureVisible();
        return MenuEvent{MenuAction::Moved, 0};
    }
    // The wheel scrolls the window a row a notch, up or down, stopping at the column's
    // ends; the selection stays where it is, the pointer choosing again only once it moves.
    if (input.pointerScroll != 0 && windowRows() < columnCount()) {
        const s32 notches = static_cast<s32>(std::round(std::abs(input.pointerScroll)));
        const s32 step = input.pointerScroll > 0 ? -1 : 1;
        return scrollWindow(m_scroll + step * notches) ? MenuEvent{MenuAction::Moved, 0}
                                                       : MenuEvent{};
    }
    return previous != m_selection ? MenuEvent{MenuAction::Moved, 0} : MenuEvent{};
}

bool OptionMenu::scrollWindow(s32 first) {
    const s32 most = std::max(0, columnCount() - windowRows());
    const s32 next = std::clamp(first, 0, most);
    if (next == m_scroll) {
        return false;
    }
    m_scroll = next;
    if (m_painter != nullptr) {
        layoutAreas(*m_painter);
    }
    return true;
}

OptionMenu::ScrollBar OptionMenu::scrollBar() const {
    ScrollBar bar;
    bar.x = static_cast<f32>(m_columnRight) + kScrollMarkGap + kScrollMarkSize;
    bar.top = static_cast<f32>(m_columnY);
    bar.bottom = static_cast<f32>(m_columnY + m_columnHeight);
    bar.trackTop = bar.top + kScrollMarkSize + kScrollMarkGap;
    bar.trackHeight = std::max(0.0f, bar.bottom - kScrollMarkSize - kScrollMarkGap - bar.trackTop);
    const s32 column = std::max(1, columnCount());
    const f32 share = static_cast<f32>(windowRows()) / static_cast<f32>(column);
    bar.thumbHeight = std::min(bar.trackHeight, std::max(kScrollMarkSize * 2, bar.trackHeight * share));
    const f32 travel = std::max(0.0f, bar.trackHeight - bar.thumbHeight);
    bar.thumbTop = bar.trackTop + travel * static_cast<f32>(m_scroll) /
                                      static_cast<f32>(std::max(1, column - windowRows()));
    return bar;
}

Rect OptionMenu::scrollBarArea() const {
    if (windowRows() >= columnCount()) {
        return {};
    }
    const ScrollBar bar = scrollBar();
    const f32 reach = kScrollMarkSize + 1;
    return Rect{bar.x - reach, bar.top - kScrollMarkGap, reach * 2,
                bar.bottom - bar.top + 2 * kScrollMarkGap};
}

s32 OptionMenu::scrollAt(f32 y) const {
    const ScrollBar bar = scrollBar();
    const s32 most = std::max(0, columnCount() - windowRows());
    const f32 travel = std::max(0.0f, bar.trackHeight - bar.thumbHeight);
    if (travel <= 0.0f) {
        return 0;
    }
    const f32 at = (y - bar.trackTop - bar.thumbHeight / 2) / travel;
    return std::clamp(static_cast<s32>(std::lround(at * static_cast<f32>(most))), 0, most);
}

void OptionMenu::focus(usize item) {
    if (item < m_definition.items.size() && m_definition.items[item].enabled) {
        m_selection = static_cast<s32>(item);
        ensureVisible();
    }
}

s32 OptionMenu::nextEnabledWithin(s32 from, s32 step) const {
    const s32 column = columnCount();
    for (s32 index = from + step; index >= 0 && index < column; index += step) {
        if (m_definition.items[static_cast<usize>(index)].enabled) {
            return index;
        }
    }
    return from;
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
    const auto column = static_cast<usize>(columnCount());
    if (index >= column && index < m_definition.items.size()) {
        return static_cast<s32>(m_definition.tailPositions[index - column].y);
    }
    if (index < m_definition.itemPositions.size()) {
        return static_cast<s32>(m_definition.itemPositions[index].y);
    }
    // Rows count from the window's first; one scrolled above it stands above the column.
    s32 y = m_columnY;
    const auto first = static_cast<usize>(m_scroll);
    for (usize i = first; i < index && i < m_definition.items.size(); ++i) {
        y += m_lineHeight + m_definition.rowGap + m_definition.items[i].extraSpacing;
    }
    for (usize i = index; i < first; ++i) {
        y -= m_lineHeight + m_definition.rowGap + m_definition.items[i].extraSpacing;
    }
    return y;
}
s32 OptionMenu::itemX(usize index) const {
    const auto column = static_cast<usize>(columnCount());
    if (index >= column && index < m_definition.items.size()) {
        return static_cast<s32>(m_definition.tailPositions[index - column].x);
    }
    return index < m_definition.itemPositions.size()
               ? static_cast<s32>(m_definition.itemPositions[index].x)
               : m_definition.x;
}

s32 OptionMenu::valueX(usize index) const {
    const auto& column = m_definition.items[index].valueColumn;
    return column ? column->x : m_definition.valueX;
}

s32 OptionMenu::valueWidth(usize index) const {
    const auto& column = m_definition.items[index].valueColumn;
    return column ? column->width : m_definition.valueWidth;
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
    return ((m_selection & 1) != 0 ? kPi : 0.0f) + m_iconRotation;
}

void OptionMenu::glideIcon(s32 ticks) {
    m_iconRotation = 0;
    if (m_definition.items.empty()) {
        return;
    }
    const auto selected = static_cast<usize>(m_selection);
    s32 target = itemY(selected) + m_lineHeight / 2;
    // A selection the wheel or the bar has scrolled out of the window is pointed at from
    // the window's nearer edge: the icon stays among the rows in view.
    if (static_cast<s32>(selected) < columnCount() && !shown(selected) && windowRows() > 0) {
        const s32 first = itemY(static_cast<usize>(m_scroll)) + m_lineHeight / 2;
        const s32 last =
            itemY(static_cast<usize>(m_scroll + windowRows() - 1)) + m_lineHeight / 2;
        target = std::clamp(target, std::min(first, last), std::max(first, last));
    }
    if (m_iconTimer == 0) {
        m_iconDrawY = m_iconY;
        if (target != m_iconY) {
            m_iconTimer = 1;
        }
    } else if (m_iconTimer < kIconGlideTicks) {
        m_iconTimer += ticks;
        m_iconDrawY = m_iconY + (target - m_iconY) * m_iconTimer / kIconGlideTicks;
        // Position and rotation use this update's interpolation, including the
        // last increment. The next update settles both, not only the position.
        m_iconRotation = kPi * static_cast<f32>(m_iconTimer) / kIconGlideTicks;
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
    // show_optmenu (GC 0x80072AA4) adds the menu and pulse transparencies.
    // In opacity space this is fade + pulse - 255, not the dimmer of the two.
    const u8 glowAlpha =
        clampByte(fade + pulseOpacity(m_time, kPulseTicks, kPulseHoldTicks) - kFullAlpha);
    const Color white = Color::white().withAlpha(fade);
    const Texture* labelSheet = m_definition.parchmentFont && textures.parchment != nullptr
                                    ? textures.parchment
                                    : textures.font;
    const Color labelColor = m_definition.parchmentFont && textures.parchment == nullptr
                                 ? m_definition.colors.off.withAlpha(fade)
                                 : white;

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
            drawLabel(canvas, painter, x, y, m_definition.title, m_definition.titleScale,
                      labelColor, labelSheet);
        }
        if (!m_definition.playerLabel.empty()) {
            const s32 y = static_cast<s32>(m_backdrop.y) + kPlayerTagMargin;
            drawLabel(canvas, painter, -kPlayerTagX, y, m_definition.playerLabel, kPromptScale,
                      labelColor, labelSheet);
        }
    }

    const bool garamondActive = m_definition.garamondIntro && textures.garamond[0] != nullptr &&
                                itemSheet(textures, false) != labelSheet;
    for (usize i = 0; i < m_definition.items.size(); ++i) {
        if (!shown(i)) {
            continue;
        }
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
                glow.color = m_definition.colors.hi.withAlpha(glowAlpha);
                glow.texture = textures.glow != nullptr ? textures.glow : textures.font;
                glow.expand = kGlowExpand;
                const bool hoverAlternate = m_hoverPart == 2 || (m_hoverPart == 0 && alternate);
                painter.draw(canvas, hoverAlternate ? secondX : x, y,
                             hoverAlternate ? item.alternate : item.text, glow);
            }
            drawLabel(canvas, painter, activeX, y, activeText + " ~", m_definition.scale,
                      m_definition.colors.on.withAlpha(fade), itemSheet(textures, true));
            continue;
        }
        const auto label = item.text + (item.markedPart == 1 ? " ~" : "");
        const bool selected = static_cast<s32>(i) == m_selection;
        const auto drawPart = [&](s32 x, const std::string& part) {
            if (selected) {
                TextStyle glow;
                glow.scale = m_definition.scale;
                glow.color = m_definition.colors.hi.withAlpha(glowAlpha);
                glow.texture = textures.glow != nullptr ? textures.glow : textures.font;
                glow.expand = kGlowExpand;
                painter.draw(canvas, x, y, part, glow);
                drawLabel(canvas, painter, x, y, part, m_definition.scale,
                          m_definition.colors.on.withAlpha(fade), itemSheet(textures, true));
            } else {
                Color color =
                    m_definition.parchmentFont && textures.parchment != nullptr && !garamondActive
                        ? white
                        : m_definition.colors.off.withAlpha(fade);
                if (!item.enabled && !item.value.empty()) {
                    color = Color::rgba(128, 128, 128, fade);
                } else if (!item.enabled) {
                    color = color.withAlpha(fade / 2);
                }
                drawLabel(canvas, painter, x, y, part, m_definition.scale, color,
                          !item.enabled && !item.value.empty() ? textures.font
                                                               : itemSheet(textures, false));
            }
        };
        drawPart(itemX(i), label);
        if (!item.value.empty()) {
            drawPart(valueX(i), item.value);
            if (item.enabled) {
                const auto center = static_cast<f32>(y) + static_cast<f32>(m_lineHeight) / 2;
                const auto left = static_cast<f32>(valueX(i)) - kArrowGap;
                const auto right = static_cast<f32>(valueX(i) + valueWidth(i)) + kArrowGap;
                const auto color =
                    (selected ? m_definition.colors.on : m_definition.colors.off).withAlpha(fade);
                canvas.fillTriangle({left - kArrowSize, center}, {left, center - kArrowSize},
                                    {left, center + kArrowSize}, color);
                canvas.fillTriangle({right + kArrowSize, center}, {right, center + kArrowSize},
                                    {right, center - kArrowSize}, color);
            }
        }
    }

    // At the column's right hand a bar shows how much of it the window holds and where,
    // with arrows above and below it marking the rows waiting there.
    if (windowRows() < columnCount()) {
        const Color ink = m_definition.colors.off.withAlpha(fade);
        const ScrollBar bar = scrollBar();
        if (m_scroll > 0) {
            canvas.fillTriangle({bar.x, bar.top}, {bar.x - kScrollMarkSize, bar.top + kScrollMarkSize},
                                {bar.x + kScrollMarkSize, bar.top + kScrollMarkSize}, ink);
        }
        if (m_scroll + windowRows() < columnCount()) {
            canvas.fillTriangle({bar.x, bar.bottom},
                                {bar.x + kScrollMarkSize, bar.bottom - kScrollMarkSize},
                                {bar.x - kScrollMarkSize, bar.bottom - kScrollMarkSize}, ink);
        }
        if (bar.trackHeight > 0) {
            canvas.fill(Rect{bar.x - kScrollBarWidth / 2, bar.trackTop, kScrollBarWidth,
                             bar.trackHeight},
                        ink.withAlpha(static_cast<u8>(fade / 3)));
            canvas.fill(Rect{bar.x - kScrollBarWidth / 2, bar.thumbTop, kScrollBarWidth,
                             bar.thumbHeight},
                        ink);
        }
    }

    if (!m_backdropReleased && !m_definition.items.empty() && m_definition.showCursor) {
        if (textures.icon != nullptr && textures.icon->bound()) {
            textures.icon->draw(
                canvas,
                Vec2{static_cast<f32>(m_columnX + kIconOffsetX), static_cast<f32>(m_iconDrawY)},
                m_iconScale, iconAngle());
        } else if (textures.arrows != nullptr) {
            const f32 width =
                static_cast<f32>(kArrowGlyphWidth * kArrowDrawScale) * m_definition.cursorScale;
            const f32 height =
                static_cast<f32>(kArrowGlyphHeight * kArrowDrawScale) * m_definition.cursorScale;
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
