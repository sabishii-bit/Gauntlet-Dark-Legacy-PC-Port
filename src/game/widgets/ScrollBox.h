#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/widgets/BurnDialogueScroll.h"
#include "game/widgets/DialoguePresentation.h"

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
    static constexpr s32 kCentreX = 256;
    static constexpr s32 kCentreY = 160;
    static constexpr s32 kMargin = 96;      ///< scroll beyond the text, both ways
    static constexpr s32 kTextInset = 32;   ///< the text's top below the scroll's
    static constexpr s32 kPromptExtra = 32; ///< the narrowest scroll beyond the prompt
    static constexpr s32 kMaxWidth = 512;
    static constexpr s32 kLineSpacing = 4;
    static constexpr s32 kPromptGap = 8;
    static constexpr s32 kButtonX = 190;
    static constexpr s32 kButtonSize = 20;
    static constexpr s32 kHoldTicks = 15; ///< before a page takes a button
    static constexpr f32 kPromptScale = 0.5f;
    static constexpr s32 kGlowPulseTicks = 40;
    static constexpr s32 kGlowHoldTicks = 5;
    static constexpr Color kGlowColor = Color::rgba(130, 0, 234);
    static constexpr Color kTextColor = Color::rgba(22, 12, 3); ///< ink on the parchment

    using PageLayout = DialoguePage;

private:
    struct Simulation {
        RenderDevice* device = nullptr;
        std::vector<std::string> pages;
        PageLayout layout;
        std::string prompt;
        f32 scale = 1.0f;
        usize page = 0;
        s32 hold = 0;
        s32 time = 0;
        bool active = false;
    };

public:
    class LocalSnapshot {
        friend class ScrollBox;
        Simulation m_state;
        const ScrollBox* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        BurnDialogueScroll::LocalSnapshot m_fire;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    void setArt(ScrollBoxArt art) {
        m_localBinding = std::make_shared<const u8>(0);
        m_burnFrames.clear();
        m_art = std::move(art);
    }
    void setText(const TextPainter* text) {
        m_localBinding = std::make_shared<const u8>(0);
        m_text = text;
    }
    void setPresentation(const DialoguePresentation* presentation) {
        close();
        m_localBinding = std::make_shared<const u8>(0);
        m_presentation = presentation;
    }

    /** Unrolls over `pages`, drawn at `scale`, with `prompt` under each; false without text
     * to draw with or a page to show. */
    bool open(RenderDevice& device, std::vector<std::string> pages, f32 scale, std::string prompt);
    void close();

    /** Steps the box; `accepted` has a bit per player who pressed their button this tick. */
    void step(s32 ticks, u32 accepted);

    bool active() const { return m_sim.active; }
    bool burning() const { return m_fire.active(); }
    /** Native-load check only; offline open/finish fallback behavior is unchanged. */
    bool burnReady() const {
        return BurnDialogueScroll::ready(m_art.backdropImage, m_art.burnMasks, m_art.burnRing);
    }
    usize page() const { return m_sim.page; }
    usize pageCount() const { return m_sim.pages.size(); }
    /** The scroll's rectangle for the current page. */
    const Rect& area() const { return m_sim.layout.area; }
    /** The lines of the current page. */
    const std::vector<std::string>& lines() const { return m_sim.layout.lines; }
    u8 promptAlpha() const;
    s32 burnFrame() const { return burning() ? m_fire.frame() : -1; }

    /** Display-only paths; never step dismissal or allocate/upload while drawing. */
    bool preloadBurn(RenderDevice& device);
    bool acceptsFrame(s32 frame) const;
    PageLayout layout(std::string_view page, f32 scale, std::string_view prompt) const;
    void drawPage(Canvas& canvas, const PageLayout& page, f32 scale, std::string_view prompt,
                  u8 promptAlpha, s32 burnFrame) const;

    /** Uploads the burn frame when the scroll is burning; call after beginFrame. */
    void prepare(RenderDevice& device);
    void draw(Canvas& canvas) const;

    /** A page's lines: split at its line breaks, a final empty line dropped. */
    static std::vector<std::string> splitLines(std::string_view page);

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    void showPage(usize page);
    void finish();
    void drawWords(Canvas& canvas, const PageLayout& page, f32 scale, std::string_view prompt,
                   u8 alpha) const;

    ScrollBoxArt m_art;
    const TextPainter* m_text = nullptr;
    const DialoguePresentation* m_presentation = nullptr;
    BurnDialogueScroll m_fire;
    std::vector<std::unique_ptr<Texture>> m_burnFrames;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
