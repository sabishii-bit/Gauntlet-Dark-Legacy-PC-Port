#pragma once

#include <optional>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"

#include "game/menu/ControlPrompts.h"
#include "game/menu/ScrollBox.h"

namespace gdl::game {

/** Level-message text and scroll presentation, including its shared bitmap painter.
 * Borrows the scene's static textures. Clear all users of text() before clear(), and
 * clear this presentation before releasing those textures. No world or audio is retained. */
class LevelMessages {
public:
    struct Cues {
        bool stopVoice = false;
        bool burnSound = false;
    };
    struct Look {
        u32 message = 0;
        u32 page = 0; ///< Absolute page in the trusted message table, including single-page opens.
        s32 burnFrame = -1;
        u8 promptAlpha = 0;
    };

    LevelMessages() = default;
    ~LevelMessages() = default;
    LevelMessages(const LevelMessages&) = delete;
    LevelMessages& operator=(const LevelMessages&) = delete;
    LevelMessages(LevelMessages&&) = delete;
    LevelMessages& operator=(LevelMessages&&) = delete;

    void load(RenderDevice& device, TextureSet& textures, const std::filesystem::path& root,
              const StringTable* strings);
    void clear();
    void setControlLabels(const ControlLabels& labels) { m_controlLabels = labels; }
    /** With no page, shows the whole message (the welcome); otherwise just that page. */
    bool open(RenderDevice& device, std::string_view name, const StringTable* strings,
              std::optional<usize> page = std::nullopt);
    /** Requests audio at the start/end of dismissal, never on ordinary page turns. */
    Cues step(s32 ticks, u32 accepted);
    void prepare(RenderDevice& device) { m_scroll.prepare(device); }
    void draw(Canvas& canvas) const { m_scroll.draw(canvas); }

    bool active() const { return m_scroll.active(); }
    const ScrollBox& scroll() const { return m_scroll; }
    const TextPainter& text() const { return m_text; }
    const MessageTable& strings() const { return m_scrollText; }
    std::optional<Look> look() const;
    bool preloadReplica(RenderDevice& device, const StringTable* strings);
    bool accepts(const Look& look) const;
    void drawReplica(Canvas& canvas, const Look& look) const;

private:
    BitmapFont m_font32;
    ControlLabels m_controlLabels;
    TextPainter m_text;
    MessageTable m_scrollText;
    ScrollBox m_scroll; ///< references our painter, so this owner must not move
    std::optional<u32> m_message;
    u32 m_firstPage = 0;
    std::string m_replicaPrompt;
    std::vector<std::vector<ScrollBox::PageLayout>> m_replicaPages;
};

} // namespace gdl::game
