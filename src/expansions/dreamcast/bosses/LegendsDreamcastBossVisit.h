#pragma once

#include <deque>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"

#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/level/LegendsDreamcastBossCeremony.h"
#include "game/effects/EffectTrees.h"

namespace gdl::game {

/** Native post-boss apparition, borrowing only this level's Dreamcast artwork.
 * Rewards and travel remain with the encounter owner; rendering is read-only. */
class LegendsDreamcastBossVisit {
public:
    void load(RenderDevice& device, ItemArchive* wizard);
    bool loadSkins(RenderDevice& device, const std::filesystem::path& nativeRoot);
    void bindStrings(const StringTable* strings) { m_strings = strings; }
    bool begin(s32 boss, s32 realm, u64 runes, u64 shards, const Vec3& anchor,
               std::span<const Vec3> players, std::optional<Vec3> rewardPosition = {});
    /** Current admitted, living players; a corrupt sample leaves the last valid one intact. */
    void updatePlayers(std::span<const Vec3> players);
    void capturePresentation();
    LegendsDreamcastBossCeremony::Events update(f32 seconds);
    void playSounds(SoundPlayer& output, LegendsDreamcastSounds& bank);
    void stopSounds();
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
              TreeModel::Pass pass, f32 alpha) const;
    void drawCaption(Canvas& canvas, const TextPainter& font, f32 width) const;
    const LegendsDreamcastBossCeremony& state() const { return m_state; }
    Vec3 attention() const { return m_position + LegendsDreamcastBossCeremony::kAttentionOffset; }
    const Vec3& position() const { return m_position; }
    f32 facing() const { return m_yaw; }
    const Texture* skin() const;
    /** What BossEnd leaves as the boss goes: its BOSSKEY, read-only. */
    const EffectTrees& rewards() const { return m_rewards; }

private:
    std::vector<std::string> pages(usize group) const;
    void facePlayer(f32 seconds, bool snap = false);
    LegendsDreamcastBossCeremony m_state;
    const StringTable* m_strings = nullptr;
    const TreeInfo* m_tree = nullptr;
    mutable TreeModel m_model;
    TextureAnimator m_textures;
    RenderDevice* m_device = nullptr;
    ItemArchive* m_rewardArchive = nullptr;
    EffectTrees m_rewards;
    std::array<std::unique_ptr<Texture>, 10> m_skins;
    f32 m_skinFrame = -1;
    AnimationPlayer m_player;
    const TreeInfo* m_runeTree = nullptr;
    mutable TreeModel m_runeModel;
    AnimationPlayer m_runePlayer;
    std::vector<usize> m_hiddenRunes;
    f32 m_previousFrame = 0;
    u64 m_previousGeneration = 0;
    f32 m_previousRuneFrame = 0;
    u64 m_previousRuneGeneration = 0;
    f32 m_previousOpacity = 0;
    Vec3 m_position{0};
    Vec3 m_anchor{0};
    f32 m_yaw = 0;
    f32 m_previousYaw = 0;
    std::vector<Vec3> m_players;
    usize m_facingPlayer = 0;
    f64 m_tickFraction = 0;
    SoundPlayer* m_output = nullptr;
    SoundHandle m_voice = kNoSound;
    std::deque<u32> m_voices;
};

} // namespace gdl::game
