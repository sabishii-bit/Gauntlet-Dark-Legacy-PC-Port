#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "game/characters/Relics.h"
#include "game/effects/EffectTrees.h"
#include "game/level/LevelWorld.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/TowerCompletion.h"

namespace gdl::game {

/** The party's tower collection and its queued return ceremonies. Borrows the tower
 * archive; clear before unloading it. Saving and acknowledging rewards belong to the scene. */
class TowerRelics {
public:
    enum class Kind : u8 { Rune, Shard, Followup };
    struct Entry {
        template <class Visitor> friend void visitNativeState(Visitor& digest, const Entry& value) {
            digest.template fields<2>(value);
        }
        Kind kind = Kind::Rune;
        s32 index = 0;
        u16 bit() const { return static_cast<u16>(1U << static_cast<u32>(index)); }
        std::string tree() const;
        std::string_view anchor() const;
        u32 camera() const;
        std::string_view voice() const;
        TowerCompletion::Kind followup() const { return static_cast<TowerCompletion::Kind>(index); }
    };
    enum class Phase : u8 { Speech, Placement, Reveal, Done };
    struct Cue {
        std::string_view voice;
        std::optional<Entry> completed;
        bool placement = false;
        std::string_view sound;
    };
    /** Queues what an arrival presents: the party's new glass and runestones with the speeches
     * that follow them, then Sumner's reminder when the level `beaten` on the way calls for
     * one. */
    void begin(std::span<const Relics> party, const MessageTable& strings,
               const std::optional<LevelBeaten>& beaten = std::nullopt);
    void bind(RenderDevice& device, LevelWorld& world, const Vec3& partyCentre);
    /** The arrival's promotions have just been spoken. SumnerDoSpeech keeps a session's camera
     * until a speech ends and the promotions end none (0x80344C74), so the speech after them,
     * whether first or after a route's reveal, keeps the promotion camera and, the promotions
     * having spent its lead, starts at once. */
    void followPromotions();
    void clear();
    /** Start a scene tick even when a cutscene will hold this presentation. */
    void capturePresentation();
    Cue update(s32 ticks, f32 seconds, bool voicePlaying);
    void animate(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const WorldCamera& camera, f32 frameBlend = -1.0f) const;
    /** Additive apparition: submit after the world's translucent scenery, with depth testing. */
    void drawWizard(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                    const WorldCamera& camera, f32 frameBlend = -1.0f) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, f32 width, f32 height) const;
    bool active() const { return m_sim.current < m_sim.entries.size(); }
    const Entry* current() const { return active() ? &m_sim.entries[m_sim.current] : nullptr; }
    Phase phase() const { return active() ? m_sim.phase : Phase::Done; }
    const std::optional<WorldCamera>& camera() const;
    u16 displayedRunes() const { return m_sim.runes; }
    u16 displayedShards() const { return m_sim.shards; }
    const EffectTrees& figures() const { return m_figures; }
    const EffectTrees& wizard() const { return m_wizard; }
    u16 pendingCeremonies() const { return m_sim.pendingCeremonies; }
    f32 revealAlpha(TowerCompletion::Kind kind) const;
    usize captionPage() const { return m_sim.page; }
    usize captionLetters() const { return static_cast<usize>(std::max(m_sim.ticks, 0) / 2); }
    std::string_view caption() const;
    static void acknowledge(Relics& relics, const Entry& entry);

private:
    f32 place(const Entry& entry, bool settled);
    void prepareSpeech();
    void prepareEntry();
    /** Queues the follow-up `kind` when its bit is pending. */
    void appendFollowup(TowerCompletion::Kind kind, const MessageTable& strings);
    void queueFollowup(TowerCompletion::Kind kind, const MessageTable& strings);
    void finish(Cue& cue);
    void updateLights();
    struct Simulation {
        std::vector<Entry> entries;
        std::vector<std::vector<std::string>> captions;
        usize page = 0;
        usize current = 0;
        u16 runes = 0;
        u16 shards = 0;
        u16 pendingCeremonies = 0;
        s32 ticks = -120;
        bool spoken = false;
        bool revealStarted = false;
        Phase phase = Phase::Speech;
        f32 placementLeft = 0;
        f32 hitIn = 0; ///< seconds until a shard's pane sounds its strike or a stone its
                       ///< landing; nought once sounded
        Vec3 wizardPosition{0};
        f32 wizardYaw = 0;
        u32 wizardMarker = 0;
        std::optional<WorldCamera> speechCamera;
        std::optional<WorldCamera> placementCamera;
    };

public:
    class LocalSnapshot {
        friend class TowerRelics;
        Simulation m_state;
        EffectTrees::LocalSnapshot m_figures;
        EffectTrees::LocalSnapshot m_wizard;
        const TowerRelics* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    RenderDevice* m_device = nullptr;
    LevelWorld* m_world = nullptr;
    EffectTrees m_figures;
    EffectTrees m_wizard;
};
} // namespace gdl::game
