#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/widgets/AudioSlider.h"
#include "game/widgets/MenuInput.h"

namespace gdl::game {

/** The virtual screen a menu lays itself out on, and the camera its 3D icon is sized for. */
struct MenuScreen {
    template <class Visitor>
    friend void visitNativeState(Visitor& digest, const MenuScreen& value) {
        const auto& [width, height, horizontalFov] = value;
        static_cast<void>(horizontalFov); // sizes only the icon drawn for this machine's camera
        digest.values(width, height);
    }
    s32 width = 512;
    s32 height = 384;
    f32 horizontalFov = glm::radians(60.0f);
};

/** Placement of a choice and its arrows independently of the other rows. */
struct MenuValueColumn {
    template <class Visitor>
    friend void visitNativeState(Visitor& digest, const MenuValueColumn& value) {
        digest.template fields<2>(value);
    }
    s32 x = 0;
    s32 width = 0;
};

struct MenuItem {
    template <class Visitor> friend void visitNativeState(Visitor& digest, const MenuItem& value) {
        digest.template fields<8>(value);
    }
    std::string text;
    s32 code = 0;
    s32 extraSpacing = 0; ///< pixels added below the item
    bool enabled = true;  ///< disabled items are greyed and skipped
    /// A second choice on the same row, e.g. Mono / Stereo.
    // NOLINTNEXTLINE(readability-redundant-member-init) -- default for partial aggregates
    std::string alternate{};
    s32 markedPart = 0; ///< 0: no mark, 1: first choice, 2: alternate choice
    // NOLINTNEXTLINE(readability-redundant-member-init) -- default for partial aggregates
    std::string value{};                                       ///< optional separate choice column
    std::optional<MenuValueColumn> valueColumn = std::nullopt; ///< overrides the shared column
};

struct MenuColors {
    template <class Visitor>
    friend void visitNativeState(Visitor& digest, const MenuColors& value) {
        digest.template fields<3>(value);
    }
    Color off = Color::rgba(180, 50, 10);
    Color on = Color::white();
    Color hi = Color::rgba(130, 0, 234);
};

struct MenuDefinition {
    template <class Visitor>
    friend void visitNativeState(Visitor& digest, const MenuDefinition& value) {
        digest.template fields<38>(value);
    }
    /** Shared parchment menu: red ink, purple selection glow, no instruction footer. */
    static MenuDefinition parchment();
    /** The retail yes/no dialog (0x8011EB1C "Quit Game?", 0x8011E8E0 "Abort Level?"): a
     * small centred parchment fading in, with only `title` and then `no` and `yes`, no first. */
    static MenuDefinition confirmation(std::string title, std::string no, std::string yes);

    std::string title; ///< drawn above the backdrop; empty for a bare menu
    f32 titleScale = 1.2f;
    s32 x = -256; ///< item x; negative centres the column on -x
    s32 y = -1;   ///< first item y; -1 centres the column vertically
    f32 scale = 1.0f;
    f32 cursorScale = 1.0f; ///< relative size of either the model or fallback arrow
    std::vector<MenuItem> items;
    std::vector<Vec2> itemPositions; ///< optional explicit layout, e.g. a horizontal action row
    /** How many rows of the column show at once; 0 shows them all. A longer column scrolls
     * behind the window as the selection moves, by the wheel too, with arrows marking what
     * lies above and below. */
    s32 visibleRows = 0;
    /** Explicit places for the last items, an action row standing outside the column and
     * its window; as many of them as the column has none of. */
    std::vector<Vec2> tailPositions;
    s32 valueX = 0;
    s32 valueWidth = 0;
    bool showCursor = true;
    MenuColors colors;
    bool startSelects = false; ///< Start confirms like Select
    bool prompts = false;      ///< draw the back / select prompt row
    std::string backLabel;
    std::string selectLabel;
    s32 promptY = 304;
    bool fades = false;         ///< fade in when opened and out when closed
    bool backdropFades = true;  ///< the backdrop fades with the text, else it stays solid
    bool parchmentFont = false; ///< unselected items use the parchment glyph sheet
    bool garamondIntro = false; ///< items flip through the ornate sheets when opened
    std::string playerLabel;    ///< drawn on the backdrop when set, e.g. "Player 1"
    std::string backdrop;       ///< texture name; empty for none
    s32 backdropX = -1;         ///< -1 sizes and centres the backdrop on the column
    s32 backdropY = -1;
    s32 backdropWidth = -1;
    s32 backdropHeight = -1;
    std::string burn; ///< animated flame overlay texture (first of five frames)
    Rect burnArea;
    /** Passages written on the backdrop in the unselected colour, each with its own line
     * breaks, centred across the screen: one after the other from `bodyY` with `bodyGap`
     * between them, or (bodyY -1) centred on the column's middle. */
    std::vector<std::string> body;
    f32 bodyScale = 1.0f;
    s32 bodyY = -1;
    s32 bodyGap = 0;
    /** The width the rows and the title must stay within, with a margin at either side: a
     * dialog's parchment, the screen. Rows wider than it shrink to fit; 0 asks nothing. */
    s32 fitWidth = 0;
    /** Pixels between the column's rows, besides each row's own spacing. */
    s32 rowGap = 0;
    /** A column centred (y -1) sits in the middle of the backdrop, under its title, rather
     * than of the screen: a dialog's rows stand evenly on its parchment. */
    bool centreOnBackdrop = false;
};

enum class MenuAction : u8 { None, Moved, Choice, Back, Closed };

struct MenuEvent {
    MenuAction action = MenuAction::None;
    s32 code = 0;
    s32 part = 0;      ///< clicked alternate choice, 1 or 2; zero for keyboard
    s32 direction = 0; ///< clicked value arrow, -1 or 1
};

/** Pulsing opacity shared by glowing text: a triangle wave with a short hold between pulses. */
u8 pulseOpacity(s32 time, s32 radius, s32 hold);

/** The glyph sheets a menu can draw with; any missing sheet falls back to `font`. */
struct MenuTextures {
    AudioSlider audioSlider;
    const Texture* font = nullptr;
    const Texture* glow = nullptr;
    const Texture* parchment = nullptr;
    const Texture* arrows = nullptr;
    std::array<const Texture*, 6> garamond{};
    const Texture* backdrop = nullptr;
    std::array<const Texture*, 5> burn{};
    const ModelSprite* icon =
        nullptr; ///< the selection arrow model; `arrows` stands in when unbound
};

/**
 * One open menu: navigation, timers and layout in the virtual screen, following the original's
 * per-tick rules (60 ticks per second).
 */
class OptionMenu {
    template <class Visitor>
    friend void visitNativeState(Visitor& digest, const OptionMenu& value) {
        const auto& [definition, screen, open, backdropReleased, selection, scroll, time,
                     finishTimer, lineHeight, columnX, columnY, columnWidth, columnRight,
                     columnHeight, titleBottom, iconScale, backdrop, iconY, iconTimer, iconDrawY,
                     iconRotation, bodyTop, itemAreas, alternateX, lastPointer, hoverPart,
                     dragging, painter] = value;
        static_cast<void>(painter);   // borrowed presentation, never simulation state
        static_cast<void>(iconScale); // drawn for this machine's own camera
        digest.values(definition, screen, open, backdropReleased, selection, scroll, time,
                      finishTimer, lineHeight, columnX, columnY, columnWidth, columnRight,
                      columnHeight, titleBottom, backdrop, iconY, iconTimer, iconDrawY,
                      iconRotation, bodyTop, itemAreas, alternateX, lastPointer, hoverPart,
                      dragging);
    }

public:
    static constexpr s32 kFadeTicks = 30;
    static constexpr s32 kPulseTicks = 40;
    static constexpr s32 kPulseHoldTicks = 5;
    static constexpr s32 kIconGlideTicks = 15;
    static constexpr s32 kBackdropMargin = 64;
    static constexpr s32 kTitleMargin = 58;
    static constexpr s32 kPlayerTagMargin = 34;
    static constexpr s32 kIconOffsetX = -16;
    static constexpr s32 kGlowExpand = 2;
    static constexpr s32 kFitMargin = 16; ///< kept clear inside `fitWidth` at either side
    static constexpr s32 kDialogRowGap = 10; ///< between a dialog's rows
    /** The lowest a row may stand under a title: the title's cell, by the painter's font. */
    s32 titleBottom() const { return m_titleBottom; }
    static constexpr f32 kPromptScale = 0.667f;
    static constexpr f32 kIconWorldScale = 0.05f; ///< the arrow model's scale in the original
    static constexpr f32 kIconDepth = 1.1f;       ///< its distance from the camera
    static constexpr f32 kScrollMarkSize = 4;     ///< the arrows marking rows beyond the window
    static constexpr s32 kScrollMarkGap = 3;      ///< between a mark and the column's edge
    static constexpr f32 kScrollBarWidth = 3;     ///< the bar between the marks
    static constexpr f32 kHoverMotion = 2;        ///< how far the pointer moves before it chooses

    /** Pixels per model unit for the selection arrow on `screen`. */
    static f32 iconPixelsPerUnit(const MenuScreen& screen);

    /** Opens the menu and lays it out with the painter's font on `screen`, the window
     * starting at column row `scroll` as far as the selection stays in it. */
    void open(const MenuDefinition& definition, const TextPainter& painter,
              const MenuScreen& screen, s32 selection = 0, s32 scroll = 0);

    /** Starts the fade-out for fading menus; removes others at once. */
    void close();
    /** Fades the current text away even when the menu appeared without an entry fade. */
    void closeWithFade();

    /** Stops drawing the backdrop, its flames and the icon; used when the backdrop is handed to
     * a burn effect while the text still fades. */
    void releaseBackdrop() { m_backdropReleased = true; }

    bool isOpen() const { return m_open; }
    bool closing() const { return m_open && m_finishTimer > 0; }
    bool backdropReleased() const { return m_backdropReleased; }

    /** Applies one frame of input; `ticks` is the elapsed tick count. */
    /** A previously resolved item choice takes the native pointer-choice path,
     * after this tick's cursor animation. No pointer or device is required. */
    MenuEvent update(const MenuInput& input, s32 ticks,
                     std::optional<usize> itemChoice = std::nullopt);
    /** Read-only local hit test against this menu's actual enabled item areas. */
    std::optional<usize> pointerChoice(const MenuInput& input) const;

    void draw(Canvas& canvas, const TextPainter& painter, const MenuTextures& textures) const;

    const MenuDefinition& definition() const { return m_definition; }
    s32 selection() const { return m_selection; }
    /** The first column row in the window. */
    s32 scroll() const { return m_scroll; }
    /** How many column rows the window shows. */
    s32 windowRows() const;
    /** Whether an item is in view: in the window, or placed outside the column. */
    bool shown(usize item) const;
    /** Moves the selection to `item` and brings the window to it. */
    void focus(usize item);
    void markItem(usize item, s32 part) { m_definition.items.at(item).markedPart = part; }
    s32 time() const { return m_time; }
    s32 finishTimer() const { return m_finishTimer; }
    s32 columnX() const { return m_columnX; }
    s32 columnWidth() const { return m_columnWidth; }
    /** The column's right edge, its value column and arrows included: the bar stands past it. */
    s32 columnRight() const { return m_columnRight; }
    /** Where the bar and its marks stand, to be clicked and dragged; empty without a window. */
    Rect scrollBarArea() const;
    /** Scrolls the window to start at column row `first`, as far as the column allows,
     * leaving the selection where it is; whether the window moved. */
    bool scrollWindow(s32 first);
    s32 columnHeight() const { return m_columnHeight; }
    s32 itemY(usize index) const;
    s32 itemX(usize index) const;
    s32 lineHeight() const { return m_lineHeight; }
    s32 iconY() const { return m_iconDrawY; }
    /** Where the body's first line is drawn. */
    s32 bodyTop() const { return m_bodyTop; }
    f32 iconScale() const { return m_iconScale; }
    Rect backdropArea() const { return m_backdrop; }
    Rect itemArea(usize index) const { return m_itemAreas.at(index); }

    /** Turn of the selection arrow about the horizontal axis: it flips over on every move. */
    f32 iconAngle() const;

    /** Alpha the whole menu is drawn with in [0, 255], from the fade in and out. */
    u8 fadeOpacity() const;

private:
    void glideIcon(s32 ticks);
    s32 nextEnabled(s32 from, s32 step) const;
    /** The next enabled column item from `from` in direction `step` without wrapping, or
     * `from` when there is none that way. */
    s32 nextEnabledWithin(s32 from, s32 step) const;
    /** How many items the column holds: all but the tail. */
    s32 columnCount() const;
    /** Scrolls the window so the selection is in it. */
    void ensureVisible();
    /** Lays the items' hit areas out for the window as it stands. */
    void layoutAreas(const TextPainter& painter);
    /** The bar's geometry: its x, the track and the thumb along it. */
    struct ScrollBar {
        f32 x = 0;
        f32 top = 0;
        f32 bottom = 0;
        f32 trackTop = 0;
        f32 trackHeight = 0;
        f32 thumbHeight = 0;
        f32 thumbTop = 0;
    };
    ScrollBar scrollBar() const;
    /** The first row the window shows with the thumb's middle at `y`. */
    s32 scrollAt(f32 y) const;
    const Texture* itemSheet(const MenuTextures& textures, bool selected) const;
    s32 valueX(usize index) const;
    s32 valueWidth(usize index) const;

    MenuDefinition m_definition;
    MenuScreen m_screen;
    bool m_open = false;
    bool m_backdropReleased = false;
    s32 m_selection = 0;
    s32 m_scroll = 0; ///< the first column row in the window
    s32 m_time = 0;
    s32 m_finishTimer = 0;
    s32 m_lineHeight = 0;
    s32 m_columnX = 0;
    s32 m_columnY = 0;
    s32 m_columnWidth = 0;
    s32 m_columnRight = 0;
    s32 m_columnHeight = 0;
    s32 m_titleBottom = 0; ///< under the title's cell, 0 without a title
    f32 m_iconScale = 0.0f;
    Rect m_backdrop;
    s32 m_iconY = 0;
    s32 m_iconTimer = kIconGlideTicks;
    s32 m_iconDrawY = 0;
    f32 m_iconRotation = 0;
    s32 m_bodyTop = 0;
    std::vector<Rect> m_itemAreas;
    std::vector<s32> m_alternateX;
    std::optional<Vec2> m_lastPointer;
    s32 m_hoverPart = 0;
    bool m_dragging = false; ///< the bar's thumb follows the pointer while held
    const TextPainter* m_painter = nullptr; ///< borrowed while open, to lay the window out again
};

} // namespace gdl::game
