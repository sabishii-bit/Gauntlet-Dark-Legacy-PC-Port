#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/audio/SpeechTiming.h"
#include "game/hud/StatusBox.h"
#include "game/scenes/GameContext.h"
#include "game/shop/ShopSession.h"

namespace gdl::game {
/** Presentation and sound for the tally/shop; ShopSession owns transactions and flow. */
class AfterLevelScene {
public:
    bool open(RenderDevice& device, const GameContext& context, std::span<const PartyMember> party,
              std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
              std::string_view levelName, ShopVisit visit = ShopVisit::Level);
    void close();
    bool isOpen() const { return m_open; }
    /** True once every lane is done and its effects' simulation-time durations have elapsed. */
    bool update(f64 seconds, const ShopSession::Inputs& inputs,
                const std::array<s32, 4>* potions = nullptr,
                ShopSession::Inputs* accepted = nullptr, bool committed = false);
    static MenuInput captureShopInput(MenuInput input, const GameConfig& config, s32 player);
    /** Translate this peer's mouse into an authenticated row request, without buying locally. */
    std::optional<usize> captureOnlinePointer(s32 seat, MenuInput& input);
    bool focusOnline(s32 seat, usize row);
    /** Ends a seat's lane for a player who dropped out of the online game. */
    bool abandonLane(s32 seat);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const ShopSession& session() const { return m_session; }
    /** The sounds the last update asked for, by name, whether or not audio is on. */
    const std::vector<std::string>& lastSounds() const { return m_lastSounds; }
    /** While a backlog of online steps is caught up, only the newest steps' menu cues are
     * heard. Level-up announcements always are, each waiting for the one before, so a backlog
     * says them in turn. Every cue keeps its timing: the shop ends on the same step anywhere. */
    void setCuesHeard(bool heard) { m_cuesHeard = heard; }

private:
    struct Effect {
        SoundHandle handle = kNoSound;
        SpeechTiming timing;
    };
    enum class PointerAction : u8 { Buy, Sell, Previous, Next, Continue };
    struct PointerTarget {
        s32 player = 0;
        ShopPhase phase = ShopPhase::Done;
        PointerAction action = PointerAction::Continue;
        usize row = 0;
        Rect area;
    };
    void pointerInput(ShopSession::Inputs& inputs, const std::array<bool, 4>& scrolling,
                      std::array<s32, 4>* focus = nullptr, s32 onlyPlayer = -1);
    void pointerTarget(const ShopLane& lane, PointerAction action, const Rect& area, usize row = 0);
    bool pointerHovered(const ShopLane& lane, PointerAction action, usize row = 0) const;
    void drawContinue(const ShopLane& lane, s32 y);
    std::string_view text(std::string_view id) const;
    const Texture* texture(std::string_view name);
    void drawLane(const ShopLane& lane);
    void drawTally(const ShopLane& lane, s32 x);
    void drawStats(const ShopLane& lane, s32 x);
    void drawFinalStats(const ShopLane& lane, s32 x);
    void drawMagicLine(const ShopLane& lane, s32 x);
    void drawShop(const ShopLane& lane, s32 x);
    void drawInventory(const ShopLane& lane, s32 x);
    /** Text in `color` with the purple glow under it when `glow`, steady while `hovered`. */
    void line(s32 x, s32 y, std::string_view value, f32 scale, Color color = Color::white(),
              bool glow = false, bool hovered = false);
    void image(std::string_view name, s32 x, s32 y, Color color = Color::white());
    void drawBackground(s32 player);
    void drawFrame(std::string_view name, s32 x, s32 y);
    void drawLaneBackdrop(const ShopLane& lane);
    void drawPile(usize pile, s32 x, f32 height);
    std::string_view rank(const ShopLane& lane) const;
    void loadVoices(std::span<const PartyMember> party);
    void playCue(const ShopEvent& event);
    void announceLevel(s32 player);
    /** Admits a native duration regardless of optional playback, preserving phrase order. */
    Effect effect(SoundSet& bank, std::string_view name, bool heard = true);
    Effect effect(SoundSet& bank, std::string_view name, const Effect& after, bool heard = true);
    void advanceEffects(f64 seconds);
    void updateTallySound();
    bool m_open = false;
    GameContext m_context;
    RenderDevice* m_device = nullptr;
    ShopSession m_session;
    TextureSet m_select;
    TextureSet m_inventory;
    TextureSet m_static;
    BitmapFont m_font;
    TextPainter m_text;
    MessageTable m_titles;
    const Texture* m_glow = nullptr;
    std::array<f32, 4> m_scroll{};
    std::array<s32, 4> m_scrollSpeed{};
    Mat4 m_pointerTransform{1};
    std::vector<PointerTarget> m_pointerTargets;
    std::array<std::optional<PointerTarget>, 4> m_hoverTargets;
    std::array<std::optional<Vec2>, 4> m_lastPointer;
    f64 m_time = 0;
    f64 m_frameTime = 0;
    StatusBoxPainter m_boxes;
    Canvas m_canvas;
    SoundSet m_musicBank;
    SoundSet m_common;
    SoundSet m_narrator;              ///< VOICE1
    SoundSet m_narratorSecond;        ///< VOICE2
    std::array<SoundSet, 4> m_voices; ///< each player's class bank, for its name
    SoundHandle m_music = kNoSound;
    SoundHandle m_tallySound = kNoSound;
    std::optional<u32> m_tallyCue;
    std::vector<SpeechTiming> m_effects;
    /** The tail of the last "has gained a level": the next one is said after it. */
    Effect m_announcement;
    std::vector<SoundHandle> m_outputEffects; ///< stopped before their borrowed banks are replaced
    std::vector<std::string> m_lastSounds;
    bool m_cuesHeard = true;
};
} // namespace gdl::game
