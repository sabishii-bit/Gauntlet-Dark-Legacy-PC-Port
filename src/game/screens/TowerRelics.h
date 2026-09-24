#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "game/players/Relics.h"
#include "game/world/EffectTrees.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** The party's tower collection and its queued return ceremonies. Borrows the tower
 * archive; clear before unloading it. Saving and acknowledging rewards belong to the scene. */
class TowerRelics {
public:
    enum class Kind : u8 { Rune, Shard };
    struct Entry {
        Kind kind = Kind::Rune;
        s32 index = 0;
        u16 bit() const { return static_cast<u16>(1U << static_cast<u32>(index)); }
        std::string tree() const;
        std::string_view anchor() const;
        u32 camera() const;
        std::string_view voice() const;
    };
    enum class Phase : u8 { Speech, Placement, Done };
    struct Cue {
        std::string_view voice;
        std::optional<Entry> completed;
        bool placement = false;
    };
    void begin(std::span<const Relics> party, const MessageTable& strings);
    void bind(RenderDevice& device, LevelWorld& world, const Vec3& partyCentre);
    void clear();
    Cue update(s32 ticks, f32 seconds, bool voicePlaying);
    void animate(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const WorldCamera& camera, bool ceremony) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, f32 width, f32 height) const;
    bool active() const { return m_current < m_entries.size(); }
    const Entry* current() const { return active() ? &m_entries[m_current] : nullptr; }
    Phase phase() const { return active() ? m_phase : Phase::Done; }
    const std::optional<WorldCamera>& camera() const;
    u16 displayedRunes() const { return m_runes; }
    u16 displayedShards() const { return m_shards; }
    const EffectTrees& figures() const { return m_figures; }
    static void acknowledge(Relics& relics, const Entry& entry);

private:
    f32 place(const Entry& entry, bool settled);
    void prepareSpeech();
    void updateLights();
    std::vector<Entry> m_entries;
    std::vector<std::string> m_captions;
    usize m_current = 0;
    u16 m_runes = 0;
    u16 m_shards = 0;
    s32 m_ticks = -120;
    bool m_spoken = false;
    Phase m_phase = Phase::Speech;
    f32 m_placementLeft = 0;
    RenderDevice* m_device = nullptr;
    LevelWorld* m_world = nullptr;
    EffectTrees m_figures;
    EffectTrees m_wizard;
    Vec3 m_wizardPosition{0};
    f32 m_wizardYaw = 0;
    u32 m_wizardMarker = 0;
    std::optional<WorldCamera> m_speechCamera;
    std::optional<WorldCamera> m_placementCamera;
};
} // namespace gdl::game
