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
#include "game/characters/Party.h"
#include "game/context/GameServices.h"
#include "game/hud/StatusBox.h"
#include "game/shop/ShopSession.h"
#include "game/widgets/PointerTargets.h"

namespace gdl::game {
/** Presentation and sound for the tally/shop; ShopSession owns transactions and flow. */
class AfterLevelScene {
public:
    /** The screen keeps reading `services`, which must outlive it. */
    bool open(RenderDevice& device, const GameServices& services,
              std::span<const PartyMember> party, std::span<const LevelResults> results,
              const std::array<s32, 3>& maxima, std::string_view levelName,
              ShopVisit visit = ShopVisit::Level);
    /** The screen keeps the services it is opened with, so they may not be a temporary. */
    bool open(RenderDevice&, const GameServices&&, std::span<const PartyMember>,
              std::span<const LevelResults>, const std::array<s32, 3>&, std::string_view,
              ShopVisit = ShopVisit::Level) = delete;
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
    /** A part of a pile on the screen and the part of its art drawn there. */
    struct PilePiece {
        Rect area;
        Rect uv;
    };
    /** How a pile `height` rows tall over the column at `x` is drawn, top to bottom: its art
     * from the top down, a row a screen row, starting over every `kPileRows` rows as the
     * GameCube's repeating textures do, and none of it above the frame. */
    static std::vector<PilePiece> pilePieces(f32 x, f32 height);
    /** The screen rows a pile's art spans before it starts over: show_piles' v of
     * height / 256 (0x803483B0). */
    static constexpr f32 kPileRows = 256;
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
    /** What a mouse control in a lane does: its seat's lane, the phase it is shown in, and
     * for Buy and Sell the catalog row. */
    struct ShopControl {
        s32 seat = 0;
        ShopPhase phase = ShopPhase::Done;
        PointerAction action = PointerAction::Continue;
        usize row = 0;
        bool operator==(const ShopControl&) const = default;
    };
    /** The part of the screen `lane`'s controls answer in: its column above the status boxes. */
    static Rect laneArea(const ShopLane& lane);
    void pointerInput(ShopSession::Inputs& inputs, const std::array<bool, 4>& scrolling,
                      std::array<s32, 4>* focus = nullptr, s32 onlyPlayer = -1);
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
    const GameServices* m_services = &GameServices::none(); ///< the game's, from open on
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
    PointerTargets<ShopControl> m_pointerTargets; ///< laid out as the lanes are drawn
    std::array<std::optional<ShopControl>, 4> m_hoverTargets;
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
