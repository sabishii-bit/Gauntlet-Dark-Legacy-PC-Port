#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "engine/core/SpecialMembers.h"

#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

/** N64 native topic menu and three-piece TEXTSCROLL modal, separate from game rules. */
class LegendsDialogue final : public DialoguePresentation {
public:
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
    void drawCeremonyCaption(Canvas& canvas, std::string_view text, f32 opacity, f32 width) const;
    f32 scale() const override { return kScale; }
    s32 textWidth() const override { return static_cast<s32>(240 * kScale); }
    usize linesPerPage() const override { return 12; }
    DialoguePage layout(std::string_view page, f32 scale, std::string_view prompt) const override;
    void draw(Canvas& canvas, const DialoguePage& page, f32 scale, std::string_view prompt,
              u8 promptAlpha) const override;

private:
    static constexpr f32 kScale = 512.0f / 416.0f;
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
