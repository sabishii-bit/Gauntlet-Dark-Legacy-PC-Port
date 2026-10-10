#pragma once

#include "game/netplay/HudSnapshot.h"
#include "game/screens/BossMeter.h"
#include "game/screens/LevelMessages.h"
#include "game/screens/PartyHud.h"
#include "game/screens/TransitionScreen.h"

namespace gdl::game {
class GameOver;
class LevelArrivalPresentation;
class HudCapture {
public:
    static std::optional<HudSnapshot> capture(std::span<const PlayerRuntime> players,
                                              const PartyHud& hud, bool visible,
                                              const BossMeters* bosses = nullptr);
    static HudScreen screen(const TransitionScreen& transition,
                            const LevelArrivalPresentation& arrival, const GameOver& gameOver,
                            bool cinematicBars);
};
/** Trusted level-loading resources. None of these identities come from packets. */
struct HudResources {
    TextureSet* counts = nullptr;
    TextureSet* bossTextures = nullptr;
    std::span<const HealthMeterReading> bossMeters;
    std::string_view levelTitle;
    ItemArchive* powerups = nullptr;
};
/** Shares the local card/selector painters; never owns PlayerRuntime or SaveSlots. */
class ReplicaHud {
public:
    ReplicaHud() = default;
    ReplicaHud(const ReplicaHud&) = delete;
    ReplicaHud& operator=(const ReplicaHud&) = delete;
    ReplicaHud(ReplicaHud&&) = delete;
    ReplicaHud& operator=(ReplicaHud&&) = delete;
    ~ReplicaHud() = default;
    bool load(RenderDevice& device, const std::filesystem::path& root, const StringTable* strings,
              const HudResources& resources = {});
    void clear();
    bool ready() const { return m_boxes.loaded() && m_text.ready(); }
    bool accepts(const HudSnapshot& snapshot) const;
    void draw(Canvas& canvas, const HudSnapshot& snapshot, const Mat4& worldToCanvas = Mat4{1});
    static StatusBoxView status(const std::optional<HudPlayer>& player);
    static std::string_view cardTexture(HudCardKind kind);
    static std::string_view countTexture(HudCountKind kind);

private:
    StatusBoxPainter m_boxes;
    BossMeters m_bosses;
    ChallengeHud m_hourglass;
    const Texture* m_runeFrame = nullptr;
    const Texture* m_runeColumn = nullptr;
    TransitionScreen m_transition;
    LevelMessages m_messages;
    MessageTable m_helpText;
    HelpMessages m_help;
    const Texture* m_helpScroll = nullptr;
    std::string m_levelTitle;
    std::string m_gameOverCaption;
    RenderDevice* m_device = nullptr;
    std::array<bool, static_cast<usize>(HudCountKind::Count)> m_countsAvailable{};
    BitmapFont m_font;
    TextureSet m_static;
    TextPainter m_text;
    const Texture* m_glow = nullptr;
    const StringTable* m_strings = nullptr;
};
} // namespace gdl::game
