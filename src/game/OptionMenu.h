#pragma once

#include <array>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/MenuInput.h"

namespace gdl::game {

/** The screen menus draw into a 512x384 virtual space. */
inline constexpr s32 kMenuScreenWidth = 512;
inline constexpr s32 kMenuScreenHeight = 384;

struct MenuItem {
    std::string text;
    s32 code = 0;
    s32 extraSpacing = 0; ///< pixels added below the item
};

struct MenuColors {
    Color off = Color::rgba(180, 50, 10);
    Color on = Color::white();
    Color hi = Color::rgba(130, 0, 234);
};

struct MenuDefinition {
    std::string title; ///< drawn above the backdrop; empty for a bare menu
    f32 titleScale = 1.2f;
    s32 x = -256; ///< item x; negative centres the column on -x
    s32 y = -1;   ///< first item y; -1 centres the column vertically
    f32 scale = 1.0f;
    std::vector<MenuItem> items;
    MenuColors colors;
    bool startSelects = false; ///< Start confirms like Select
    bool prompts = false;      ///< draw the "Back" / "Select" row
    s32 promptY = 304;
    bool fades = false;         ///< fade in when opened and out when closed
    bool parchmentFont = false; ///< unselected items use the parchment glyph sheet
    bool garamondIntro = false; ///< items flip through the ornate sheets when opened
    bool playerTag = false;     ///< "Player 1" label on the backdrop
    std::string backdrop;       ///< texture name; empty for none
    s32 backdropX = -1;         ///< -1 sizes and centres the backdrop on the column
    s32 backdropY = -1;
    s32 backdropWidth = -1;
    s32 backdropHeight = -1;
    std::string burn; ///< animated flame overlay texture (first of five frames)
    Rect burnArea;
};

enum class MenuAction : u8 { None, Moved, Choice, Back, Closed };

struct MenuEvent {
    MenuAction action = MenuAction::None;
    s32 code = 0;
};

/** Pulsing opacity shared by glowing text: a triangle wave with a short hold between pulses. */
u8 pulseOpacity(s32 time, s32 radius, s32 hold);

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
    static constexpr s32 kFadeTicks = 30;
    static constexpr s32 kPulseTicks = 40;
    static constexpr s32 kPulseHoldTicks = 5;
    static constexpr s32 kIconGlideTicks = 15;
    static constexpr s32 kBackdropMargin = 64;
    static constexpr s32 kTitleMargin = 58;
    static constexpr s32 kPlayerTagMargin = 34;
    static constexpr s32 kIconOffsetX = -16;
    static constexpr s32 kGlowExpand = 2;
    static constexpr f32 kIconPixelsPerUnit = 10.0f;
    static constexpr f32 kPromptScale = 0.667f;

    /** Opens the menu and lays it out with the painter's font. */
    void open(const MenuDefinition& definition, const TextPainter& painter, s32 selection = 0);

    /** Starts the fade-out for fading menus; removes others at once. */
    void close();

    bool isOpen() const { return m_open; }
    bool closing() const { return m_open && m_finishTimer > 0; }

    /** Applies one frame of input; `ticks` is the elapsed 60 Hz tick count. */
    MenuEvent update(const MenuInput& input, s32 ticks);

    void draw(Canvas& canvas, const TextPainter& painter, const MenuTextures& textures) const;

    const MenuDefinition& definition() const { return m_definition; }
    s32 selection() const { return m_selection; }
    s32 time() const { return m_time; }
    s32 finishTimer() const { return m_finishTimer; }
    s32 columnX() const { return m_columnX; }
    s32 columnWidth() const { return m_columnWidth; }
    s32 columnHeight() const { return m_columnHeight; }
    s32 itemY(usize index) const;
    s32 lineHeight() const { return m_lineHeight; }
    s32 iconY() const { return m_iconDrawY; }

    /** Turn of the selection arrow about the horizontal axis: it flips over on every move. */
    f32 iconAngle() const;
    Rect backdropArea() const { return m_backdrop; }

    /** Alpha the whole menu is drawn with in [0, 255], from the fade in and out. */
    u8 fadeOpacity() const;

private:
    void glideIcon(s32 ticks);
    const Texture* itemSheet(const MenuTextures& textures, bool selected) const;

    MenuDefinition m_definition;
    bool m_open = false;
    s32 m_selection = 0;
    s32 m_time = 0;
    s32 m_finishTimer = 0;
    s32 m_lineHeight = 0;
    s32 m_columnX = 0;
    s32 m_columnY = 0;
    s32 m_columnWidth = 0;
    s32 m_columnHeight = 0;
    Rect m_backdrop;
    s32 m_iconY = 0;
    s32 m_iconTimer = kIconGlideTicks;
    s32 m_iconDrawY = 0;
};

} // namespace gdl::game
