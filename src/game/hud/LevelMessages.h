#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"

#include "game/widgets/ScrollBox.h"

namespace gdl::game {

/** Level-message text and scroll presentation, including its shared bitmap painter.
 * Borrows the scene's static textures. Clear all users of text() before clear(), and
 * clear this presentation before releasing those textures. No world or audio is retained. */
class LevelMessages {
    struct Simulation {
        std::optional<u32> message;
        u32 firstPage = 0;
    };

public:
    class LocalSnapshot {
        friend class LevelMessages;
        Simulation m_state;
        const LevelMessages* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        ScrollBox::LocalSnapshot m_scroll;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
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
    /** Borrowed for this level only; does not replace the HUD's shared font. */
    void setPresentation(const DialoguePresentation* presentation) {
        m_presentation = presentation;
        m_scroll.setPresentation(presentation);
    }
    /** With no page, shows the whole message (the welcome); otherwise just that page. */
    bool open(RenderDevice& device, std::string_view name, const StringTable* strings,
              std::optional<usize> page = std::nullopt);
    /** Imported native prose uses this scroll UI without borrowing GC message IDs. */
    bool openImported(RenderDevice& device, std::span<const std::string> text,
                      const StringTable* strings);
    /** The game's own pages, already broken into lines, drawn as the rom's message `style` is
     * (at its scale); false while a scroll is up, or without that message or a page. */
    bool openStyled(RenderDevice& device, std::string_view style, std::vector<std::string> pages,
                    const StringTable* strings);
    /** Requests audio at the start/end of dismissal, never on ordinary page turns. */
    Cues step(s32 ticks, u32 accepted);
    void prepare(RenderDevice& device) { m_scroll.prepare(device); }
    void draw(Canvas& canvas) const { m_scroll.draw(canvas); }

    bool active() const { return m_scroll.active(); }
    /** Native peers must not choose different gameplay holds due to optional
     * font or burn-art load failures. Offline callers may still omit this check. */
    bool nativeReady() const {
        return m_scrollText.loaded() && m_text.ready() && m_scroll.burnReady();
    }
    const ScrollBox& scroll() const { return m_scroll; }
    const TextPainter& text() const { return m_text; }
    const MessageTable& strings() const { return m_scrollText; }
    std::optional<Look> look() const;

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    BitmapFont m_font32;
    TextPainter m_text;
    MessageTable m_scrollText;
    ScrollBox m_scroll; ///< references our painter, so this owner must not move
    const DialoguePresentation* m_presentation = nullptr;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
