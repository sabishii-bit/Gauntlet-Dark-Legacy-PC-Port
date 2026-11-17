#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"

#include "expansions/n64/hud/LegendsN64Frame.h"
#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

/** N64 native topic menu and three-piece TEXTSCROLL modal, separate from game rules. Popups and
 * Tower captions draw in the N64's frame (LegendsN64Frame), a page's geometry in its units, and
 * the topic menu's scroll, title and rows stand where the frame places them. */
class LegendsDialogue final : public DialoguePresentation {
public:
    /** The topic menu's glyph scale, the frame's width fitted to the canvas's: OptionMenu draws
     * its rows at one scale, so their glyphs cannot take the frame's own proportions. */
    static constexpr f32 kTopicGlyphScale = LegendsN64Frame::kCanvasWidth / LegendsN64Frame::kWidth;

    LegendsDialogue() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsDialogue);
    ~LegendsDialogue() override = default;
    bool load(RenderDevice& device, const std::filesystem::path& nativeRoot);
    const TextPainter& text() const override { return m_text; }
    const MenuTextures* menuTextures() const override { return &m_menuTextures; }
    std::optional<MenuDefinition> nativeTopics(std::string_view title,
                                               std::span<const MenuItem> topics) const override;
    bool drawTopicsBackdrop(Canvas& canvas, const OptionMenu& menu,
                            std::string_view title) const override;
    /** e086098c's automatic Tower captions, distinct from modal scrolls; the
     * caller looks the line up in the string table. */
    void drawCeremonyCaption(Canvas& canvas, std::string_view text, f32 opacity) const;
    /** Pages are measured and drawn in the frame's own units. */
    f32 scale() const override { return 1; }
    /** The N64 breaks a line once it reaches the wrap (game.bin FUN_e00189c0), the shared pager
     * only once it passes this width. */
    s32 textWidth() const override { return kWrap - 1; }
    usize linesPerPage() const override { return 12; }
    DialoguePage layout(std::string_view page, f32 scale, std::string_view prompt) const override;
    void draw(Canvas& canvas, const DialoguePage& page, f32 scale, std::string_view prompt,
              u8 promptAlpha) const override;

private:
    /** e00717a0's wrap for its 350-wide long-page scroll, the last of the wraps at e00ac0cc. */
    static constexpr s32 kWrap = 240;
    BitmapFont m_font;
    BitmapFont m_titleFont;
    std::unique_ptr<Texture> m_fontSheet;
    std::unique_ptr<Texture> m_titleFontSheet;
    std::unique_ptr<Texture> m_scroll;
    TextPainter m_text;
    TextPainter m_titleText;
    MenuTextures m_menuTextures;
};

} // namespace gdl::game
