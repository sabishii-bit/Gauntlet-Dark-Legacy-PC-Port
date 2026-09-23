#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/MenuInput.h"

namespace gdl::game {

/** The virtual screen a menu lays itself out on, and the camera its 3D icon is sized for. */
struct MenuScreen {
    int width = 512;
    int height = 384;
    float horizontalFov = glm::radians(60.0f);
};

struct MenuItem {
    std::string text;
    int code = 0;
    int extraSpacing = 0; ///< pixels added below the item
    bool enabled = true;  ///< disabled items are greyed and skipped
};

struct MenuColors {
    Color off = Color::rgba(180, 50, 10);
    Color on = Color::white();
    Color hi = Color::rgba(130, 0, 234);
};

struct MenuDefinition {
    std::string title; ///< drawn above the backdrop; empty for a bare menu
    float titleScale = 1.2f;
    int x = -256; ///< item x; negative centres the column on -x
    int y = -1;   ///< first item y; -1 centres the column vertically
    float scale = 1.0f;
    std::vector<MenuItem> items;
    MenuColors colors;
    bool startSelects = false; ///< Start confirms like Select
    bool prompts = false;      ///< draw the back / select prompt row
    std::string backLabel;
    std::string selectLabel;
    int promptY = 304;
    bool fades = false;         ///< fade in when opened and out when closed
    bool backdropFades = true;  ///< the backdrop fades with the text, else it stays solid
    bool parchmentFont = false; ///< unselected items use the parchment glyph sheet
    bool garamondIntro = false; ///< items flip through the ornate sheets when opened
    std::string playerLabel;    ///< drawn on the backdrop when set, e.g. "Player 1"
    std::string backdrop;       ///< texture name; empty for none
    int backdropX = -1;         ///< -1 sizes and centres the backdrop on the column
    int backdropY = -1;
    int backdropWidth = -1;
    int backdropHeight = -1;
    std::string burn; ///< animated flame overlay texture (first of five frames)
    Rect burnArea;
    /** Passages written on the backdrop in the unselected colour, each with its own line
     * breaks, centred across the screen: one after the other from `bodyY` with `bodyGap`
     * between them, or (bodyY -1) centred on the column's middle. */
    std::vector<std::string> body;
    float bodyScale = 1.0f;
    int bodyY = -1;
    int bodyGap = 0;
};

enum class MenuAction : std::uint8_t { None, Moved, Choice, Back, Closed };

struct MenuEvent {
    MenuAction action = MenuAction::None;
    int code = 0;
};

/** Pulsing opacity shared by glowing text: a triangle wave with a short hold between pulses. */
std::uint8_t pulseOpacity(int time, int radius, int hold);

/** The glyph sheets a menu can draw with; any missing sheet falls back to `font`. */
struct MenuTextures {
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
public:
    static constexpr int kFadeTicks = 30;
    static constexpr int kPulseTicks = 40;
    static constexpr int kPulseHoldTicks = 5;
    static constexpr int kIconGlideTicks = 15;
    static constexpr int kBackdropMargin = 64;
    static constexpr int kTitleMargin = 58;
    static constexpr int kPlayerTagMargin = 34;
    static constexpr int kIconOffsetX = -16;
    static constexpr int kGlowExpand = 2;
    static constexpr float kPromptScale = 0.667f;
    static constexpr float kIconWorldScale = 0.05f; ///< the arrow model's scale in the original
    static constexpr float kIconDepth = 1.1f;       ///< its distance from the camera

    /** Pixels per model unit for the selection arrow on `screen`. */
    static float iconPixelsPerUnit(const MenuScreen& screen);

    /** Opens the menu and lays it out with the painter's font on `screen`. */
    void open(const MenuDefinition& definition, const TextPainter& painter,
              const MenuScreen& screen, int selection = 0);

    /** Starts the fade-out for fading menus; removes others at once. */
    void close();

    /** Stops drawing the backdrop, its flames and the icon; used when the backdrop is handed to
     * a burn effect while the text still fades. */
    void releaseBackdrop() { m_backdropReleased = true; }

    bool isOpen() const { return m_open; }
    bool closing() const { return m_open && m_finishTimer > 0; }
    bool backdropReleased() const { return m_backdropReleased; }

    /** Applies one frame of input; `ticks` is the elapsed tick count. */
    MenuEvent update(const MenuInput& input, int ticks);

    void draw(Canvas& canvas, const TextPainter& painter, const MenuTextures& textures) const;

    const MenuDefinition& definition() const { return m_definition; }
    int selection() const { return m_selection; }
    int time() const { return m_time; }
    int finishTimer() const { return m_finishTimer; }
    int columnX() const { return m_columnX; }
    int columnWidth() const { return m_columnWidth; }
    int columnHeight() const { return m_columnHeight; }
    int itemY(std::size_t index) const;
    int lineHeight() const { return m_lineHeight; }
    int iconY() const { return m_iconDrawY; }
    /** Where the body's first line is drawn. */
    int bodyTop() const { return m_bodyTop; }
    float iconScale() const { return m_iconScale; }
    Rect backdropArea() const { return m_backdrop; }

    /** Turn of the selection arrow about the horizontal axis: it flips over on every move. */
    float iconAngle() const;

    /** Alpha the whole menu is drawn with in [0, 255], from the fade in and out. */
    std::uint8_t fadeOpacity() const;

private:
    void glideIcon(int ticks);
    int nextEnabled(int from, int step) const;
    const Texture* itemSheet(const MenuTextures& textures, bool selected) const;

    MenuDefinition m_definition;
    MenuScreen m_screen;
    bool m_open = false;
    bool m_backdropReleased = false;
    int m_selection = 0;
    int m_time = 0;
    int m_finishTimer = 0;
    int m_lineHeight = 0;
    int m_columnX = 0;
    int m_columnY = 0;
    int m_columnWidth = 0;
    int m_columnHeight = 0;
    float m_iconScale = 0.0f;
    Rect m_backdrop;
    int m_iconY = 0;
    int m_iconTimer = kIconGlideTicks;
    int m_iconDrawY = 0;
    int m_bodyTop = 0;
};

} // namespace gdl::game
