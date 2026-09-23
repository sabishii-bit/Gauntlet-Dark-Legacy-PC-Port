#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/BurnDialogueScroll.h"
#include "game/menu/MenuInput.h"
#include "game/menu/OptionMenu.h"

namespace gdl::game {

/** What the hint scroll draws and burns with. */
struct HintMenuArt {
    MenuTextures textures;
    const Image* scroll = nullptr; ///< the backdrop's image, for burning it away
    std::vector<const Image*> burnMasks;
    std::vector<const Texture*> burnRing;
};

/** The scroll's words. */
struct HintMenuLabels {
    std::string title;
    std::vector<MenuItem> topics; ///< what can be asked, each with the code it answers to
    std::string back;
    std::string select;
    std::string player; ///< whose scroll it is, e.g. "Player 1"
};

/** What a step of the scroll did, for the sounds and the answer it wants. */
struct HintMenuEvent {
    enum class Kind : std::uint8_t { None, Moved, Asked, Returned, Left };
    Kind kind = Kind::None;
    std::int32_t topic = 0; ///< the asked topic's code
};

/**
 * The scroll Sumner holds out to a player who steps up to him, laid out as the original's:
 * a list of topics under his question, each answered by a page of the same scroll (a title,
 * the hint's passages in ink, Back to return), and the scroll burning away once the player
 * backs out of the list.
 */
class HintMenu {
public:
    static constexpr std::int32_t kTopicsX = 128;
    static constexpr std::int32_t kBackdropY = 8;
    static constexpr std::int32_t kBackdropWidth = 480;
    static constexpr std::int32_t kBackdropHeight = 360;
    static constexpr std::int32_t kPromptY = 304;
    static constexpr float kTitleScale = 1.2f;
    static constexpr float kPageTitleScale = 0.8f;
    static constexpr float kPageScale = 0.667f;
    static constexpr std::int32_t kPageTop =
        112; ///< where a page's passages start when not centred
    static constexpr Color kInk = Color::rgba(92, 26, 3);

    void setArt(HintMenuArt art) { m_art = std::move(art); }

    /** Unrolls the list of topics; false without a font to draw with or a topic to ask. */
    bool open(const TextPainter& painter, const MenuScreen& screen, HintMenuLabels labels);
    /** Shows the answer to the topic just asked. */
    void read(const TextPainter& painter, std::string title, std::vector<std::string> passages,
              float scale, bool centred, std::int32_t gap);
    /** Drops everything at once. */
    void close();

    /** Up while the list, a page or the burning scroll shows. */
    bool active() const { return m_topics.isOpen() || m_page.isOpen() || m_fire.active(); }
    bool reading() const { return m_page.isOpen() && !m_page.closing(); }
    bool burning() const { return m_fire.active(); }
    const OptionMenu& topics() const { return m_topics; }
    const OptionMenu& page() const { return m_page; }

    /** Steps the scroll with its player's input. */
    HintMenuEvent update(RenderDevice& device, const MenuInput& input, std::int32_t ticks);

    /** Uploads the burn frame when the scroll is burning; call after beginFrame. */
    void prepare(RenderDevice& device);
    void draw(Canvas& canvas, const TextPainter& painter) const;

private:
    MenuDefinition topicsDefinition(bool fadeBackdrop) const;
    void leave(RenderDevice& device);

    HintMenuArt m_art;
    HintMenuLabels m_labels;
    MenuScreen m_screen;
    const TextPainter* m_painter = nullptr;
    OptionMenu m_topics;
    OptionMenu m_page;
    BurnDialogueScroll m_fire;
};

} // namespace gdl::game
