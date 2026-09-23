#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/BurnDialogueScroll.h"

namespace gdl::game {

/** What the scroll draws with. */
struct ScrollBoxArt {
    const Texture* backdrop = nullptr;    ///< the scroll sheet
    const Image* backdropImage = nullptr; ///< the same, for burning it away
    const Texture* glow = nullptr;        ///< the prompt's glow sheet
    const Texture* button = nullptr;      ///< the icon in the prompt
    std::vector<const Image*> burnMasks;
    std::vector<const Texture*> burnRing;
};

/**
 * The scroll that unrolls over the game to deliver a message page by page, as the original's
 * message box does: the scroll sized to the page's text plus its margins and centred on the
 * virtual screen, the text centred inside, a prompt with a button icon under it, the next
 * page on any joined player's button once the page has been up fifteen ticks, and the scroll
 * burning away after the last page.
 */
class ScrollBox {
public:
    static constexpr std::int32_t kCentreX = 256;
    static constexpr std::int32_t kCentreY = 160;
    static constexpr std::int32_t kMargin = 96;      ///< scroll beyond the text, both ways
    static constexpr std::int32_t kTextInset = 32;   ///< the text's top below the scroll's
    static constexpr std::int32_t kPromptExtra = 32; ///< the narrowest scroll beyond the prompt
    static constexpr std::int32_t kMaxWidth = 512;
    static constexpr std::int32_t kLineSpacing = 4;
    static constexpr std::int32_t kPromptGap = 8;
    static constexpr std::int32_t kButtonX = 190;
    static constexpr std::int32_t kButtonSize = 20;
    static constexpr std::int32_t kHoldTicks = 15; ///< before a page takes a button
    static constexpr float kPromptScale = 0.5f;
    static constexpr std::int32_t kGlowPulseTicks = 40;
    static constexpr std::int32_t kGlowHoldTicks = 5;
    static constexpr Color kGlowColor = Color::rgba(130, 0, 234);
    static constexpr Color kTextColor = Color::rgba(22, 12, 3); ///< ink on the parchment

    void setArt(ScrollBoxArt art) { m_art = std::move(art); }
    void setText(const TextPainter* text) { m_text = text; }

    /** Unrolls over `pages`, drawn at `scale`, with `prompt` under each; false without text
     * to draw with or a page to show. */
    bool open(RenderDevice& device, std::vector<std::string> pages, float scale,
              std::string prompt);
    void close();

    /** Steps the box; `accepted` has a bit per player who pressed their button this tick. */
    void step(std::int32_t ticks, std::uint32_t accepted);

    bool active() const { return m_active; }
    bool burning() const { return m_fire.active(); }
    std::size_t page() const { return m_page; }
    std::size_t pageCount() const { return m_pages.size(); }
    /** The scroll's rectangle for the current page. */
    const Rect& area() const { return m_area; }
    /** The lines of the current page. */
    const std::vector<std::string>& lines() const { return m_lines; }

    /** Uploads the burn frame when the scroll is burning; call after beginFrame. */
    void prepare(RenderDevice& device);
    void draw(Canvas& canvas) const;

    /** A page's lines: split at its line breaks, a final empty line dropped. */
    static std::vector<std::string> splitLines(std::string_view page);

private:
    void showPage(std::size_t page);
    void finish();

    ScrollBoxArt m_art;
    const TextPainter* m_text = nullptr;
    RenderDevice* m_device = nullptr;
    std::vector<std::string> m_pages;
    std::vector<std::string> m_lines;
    std::string m_prompt;
    float m_scale = 1.0f;
    std::size_t m_page = 0;
    Rect m_area;
    std::int32_t m_textTop = 0;
    std::int32_t m_promptY = 0;
    std::int32_t m_hold = 0;
    std::int32_t m_time = 0;
    bool m_active = false;
    BurnDialogueScroll m_fire;
};

} // namespace gdl::game
