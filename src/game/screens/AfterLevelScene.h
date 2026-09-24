#pragma once
#include <optional>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/screens/GameContext.h"
#include "game/screens/ShopSession.h"
#include "game/screens/StatusBox.h"

namespace gdl::game {
/** Presentation and sound for the tally/shop; ShopSession owns transactions and flow. */
class AfterLevelScene {
public:
    bool open(RenderDevice& device, const GameContext& context, std::span<const PartyMember> party,
              std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
              std::string_view levelName, bool towerShop = false);
    void close();
    bool isOpen() const { return m_open; }
    bool update(f64 seconds, const ShopSession::Inputs& inputs);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const ShopSession& session() const { return m_session; }

private:
    std::string_view text(std::string_view id) const;
    const Texture* texture(std::string_view name);
    void drawLane(const ShopLane& lane);
    void drawTally(const ShopLane& lane, s32 x);
    void drawStats(const ShopLane& lane, s32 x);
    void drawShop(const ShopLane& lane, s32 x);
    void line(s32 x, s32 y, std::string_view value, f32 scale, Color color = Color::white(),
              bool glow = false);
    void image(std::string_view name, s32 x, s32 y, Color color = Color::white());
    void prompt(s32 x, s32 y, s32 size);
    void drawBackground(s32 player);
    void drawPile(usize pile, s32 x, f32 height);
    std::string_view rank(const ShopLane& lane) const;
    void sound(std::string_view name);
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
    f64 m_time = 0;
    StatusBoxPainter m_boxes;
    Canvas m_canvas;
    SoundSet m_musicBank;
    SoundSet m_common;
    SoundHandle m_music = kNoSound;
    SoundHandle m_tallySound = kNoSound;
    std::optional<u32> m_tallyCue;
};
} // namespace gdl::game
