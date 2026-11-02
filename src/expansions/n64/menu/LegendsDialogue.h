#pragma once

#include <filesystem>
#include <memory>

#include "engine/core/SpecialMembers.h"

#include "game/menu/DialoguePresentation.h"

namespace gdl::game {

/** N64 font5 and three-piece TEXTSCROLL modal, owned independently of game rules. */
class LegendsDialogue final : public DialoguePresentation {
public:
    LegendsDialogue() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsDialogue);
    ~LegendsDialogue() override = default;
    bool load(RenderDevice& device, const std::filesystem::path& nativeRoot);
    const TextPainter& text() const override { return m_text; }
    f32 scale() const override { return kScale; }
    s32 textWidth() const override { return static_cast<s32>(240 * kScale); }
    usize linesPerPage() const override { return 12; }
    DialoguePage layout(std::string_view page, f32 scale, std::string_view prompt) const override;
    void draw(Canvas& canvas, const DialoguePage& page, f32 scale, std::string_view prompt,
              u8 promptAlpha) const override;

private:
    static constexpr f32 kScale = 512.0f / 416.0f;
    BitmapFont m_font;
    std::unique_ptr<Texture> m_fontSheet;
    std::unique_ptr<Texture> m_scroll;
    TextPainter m_text;
};

} // namespace gdl::game
