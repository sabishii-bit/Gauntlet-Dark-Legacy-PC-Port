#pragma once

#include <cstddef>
#include <optional>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/TextureSet.h"

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

    LevelMessages() = default;
    ~LevelMessages() = default;
    LevelMessages(const LevelMessages&) = delete;
    LevelMessages& operator=(const LevelMessages&) = delete;
    LevelMessages(LevelMessages&&) = delete;
    LevelMessages& operator=(LevelMessages&&) = delete;

    void load(RenderDevice& device, TextureSet& textures, const std::filesystem::path& root,
              const StringTable* strings);
    void clear();
    /** With no page, shows the whole message (the welcome); otherwise just that page. */
    bool open(RenderDevice& device, std::string_view name, const StringTable* strings,
              std::optional<std::size_t> page = std::nullopt);
    /** Requests audio at the start/end of dismissal, never on ordinary page turns. */
    Cues step(int ticks, unsigned int accepted);
    void prepare(RenderDevice& device) { m_scroll.prepare(device); }
    void draw(Canvas& canvas) const { m_scroll.draw(canvas); }

    bool active() const { return m_scroll.active(); }
    const ScrollBox& scroll() const { return m_scroll; }
    const TextPainter& text() const { return m_text; }

private:
    BitmapFont m_font32;
    TextPainter m_text;
    MessageTable m_scrollText;
    ScrollBox m_scroll; ///< references our painter, so this owner must not move
};

} // namespace gdl::game
