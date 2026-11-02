#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/widgets/BurnDialogueScroll.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/OptionMenu.h"

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
    std::string select;
    std::string player; ///< whose scroll it is, e.g. "Player 1"
};

/** What a step of the scroll did, for the sounds and the answer it wants. */
struct HintMenuEvent {
    enum class Kind : u8 { None, Moved, Asked, Returned, Left };
    Kind kind = Kind::None;
    s32 topic = 0; ///< the asked topic's code
};

/**
 * The scroll Sumner holds out to a player who steps up to him, laid out as the original's:
 * a list of topics under his question, each answered by a page of the same scroll (a title,
 * the hint's passages in ink, Back to return), and the scroll burning away once the player
 * backs out of the list.
 */
class HintMenu {
    struct Simulation {
        HintMenuLabels labels;
        MenuScreen screen;
        const TextPainter* painter = nullptr;
        OptionMenu topics;
        OptionMenu page;
        u8 revision = 0; ///< bounded input window is shorter than a complete revision cycle
    };

public:
    class LocalSnapshot {
        friend class HintMenu;
        Simulation m_state;
        const HintMenu* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        BurnDialogueScroll::LocalSnapshot m_fire;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr s32 kTopicsX = 128;
    static constexpr s32 kBackdropY = 8;
    static constexpr s32 kBackdropWidth = 480;
    static constexpr s32 kBackdropHeight = 360;
    static constexpr s32 kPromptY = 304;
    static constexpr f32 kTitleScale = 1.2f;
    static constexpr f32 kPageTitleScale = 0.8f;
    static constexpr f32 kPageScale = 0.667f;
    static constexpr s32 kPageTop = 112; ///< where a page's passages start when not centred
    static constexpr Color kInk = Color::rgba(92, 26, 3);

    void setArt(HintMenuArt art) {
        m_localBinding = std::make_shared<const u8>(0);
        m_art = std::move(art);
    }

    /** Unrolls the list of topics; false without a font to draw with or a topic to ask. */
    bool open(const TextPainter& painter, const MenuScreen& screen, HintMenuLabels labels);
    /** Shows the answer to the topic just asked. */
    void read(const TextPainter& painter, std::string title, std::vector<std::string> passages,
              f32 scale, bool centred, s32 gap);
    /** Drops everything at once. */
    void close();

    /** Up while the list, a page or the burning scroll shows. */
    bool active() const { return m_sim.topics.isOpen() || m_sim.page.isOpen() || m_fire.active(); }
    bool reading() const { return m_sim.page.isOpen() && !m_sim.page.closing(); }
    bool burning() const { return m_fire.active(); }
    /** Native-load check only; offline missing-art fallback remains supported. */
    bool burnReady() const {
        return BurnDialogueScroll::ready(m_art.scroll, m_art.burnMasks, m_art.burnRing);
    }
    const OptionMenu& topics() const { return m_sim.topics; }
    const OptionMenu& page() const { return m_sim.page; }
    u8 revision() const { return m_sim.revision; }
    /** Wire row zero is absent; the four native topics occupy rows one to four. */
    u8 captureTopic(const MenuInput& input) const;
    bool acceptsTopic(u8 topic, u8 revision) const;

    /** Steps the scroll with its player's input. */
    HintMenuEvent update(RenderDevice& device, const MenuInput& input, s32 ticks, u8 topic = 0,
                         u8 revision = 0);

    /** Uploads the burn frame when the scroll is burning; call after beginFrame. */
    void prepare(RenderDevice& device);
    void draw(Canvas& canvas, const TextPainter& painter) const;

private:
    void appendState(NativeStateDigest& digest, const Simulation& state) const;
    MenuDefinition topicsDefinition(bool fadeBackdrop) const;
    void leave(RenderDevice& device);

    HintMenuArt m_art;
    BurnDialogueScroll m_fire;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
