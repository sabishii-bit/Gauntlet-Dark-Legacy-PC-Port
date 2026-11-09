#pragma once

#include <filesystem>
#include <memory>

#include "engine/core/SpecialMembers.h"

#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

/** Dreamcast's native scroll and font tables, borrowed by the shared menu controllers. */
class LegendsDreamcastDialogue final : public DialoguePresentation {
public:
    LegendsDreamcastDialogue() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsDreamcastDialogue);
    ~LegendsDreamcastDialogue() override = default;

    bool load(RenderDevice& device, const std::filesystem::path& nativeRoot);
    const TextPainter& text() const override { return m_text; }
    const TextPainter& menuText() const override { return m_menuText; }
    const MenuTextures* menuTextures() const override { return &m_menuTextures; }
    std::optional<MenuDefinition> nativeTopics(std::string_view title,
                                               std::span<const MenuItem> topics) const override;
    f32 scale() const override { return kScale; }
    s32 textWidth() const override { return 320; }
    usize linesPerPage() const override { return 10; }
    DialoguePage layout(std::string_view page, f32 scale, std::string_view prompt) const override;
    void draw(Canvas& canvas, const DialoguePage& page, f32 scale, std::string_view prompt,
              u8 promptAlpha) const override;

private:
    static constexpr f32 kScale = 1.25f;
    BitmapFont m_font;
    BitmapFont m_menuFont;
    std::unique_ptr<Texture> m_fontSheet;
    std::unique_ptr<Texture> m_menuFontSheet;
    std::unique_ptr<Texture> m_scroll;
    std::unique_ptr<Texture> m_arrows;
    TextPainter m_text;
    TextPainter m_menuText;
    MenuTextures m_menuTextures;
};

} // namespace gdl::game
