#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/MessageTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCamera.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {
/** Tower-return awards: one speech per eligible character, with authored wizard/camera marks.
 * Borrows tower artwork; experience and save ownership stay with the party. */
class TowerPromotion {
public:
    struct Cue {
        bool voice = false;
        bool award = false;
        bool gem = false;
    };
    struct Entry {
        usize player = 0;
        s32 level = 0;
        std::string caption;
        std::string voice;
    };
    void begin(std::span<const PlayerRuntime> players, const MessageTable& strings);
    void bind(RenderDevice& device, ItemArchive& items, const WorldLayout& layout,
              std::span<const PlayerRuntime> players);
    void clear();
    Cue update(s32 ticks, bool voicePlaying);
    void animate(f32 seconds);
    bool active() const { return m_current < m_entries.size(); }
    const Entry* current() const { return active() ? &m_entries[m_current] : nullptr; }
    const std::optional<WorldCamera>& camera() const { return m_camera; }
    usize shown() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, f32 width, f32 height) const;
    static std::string caption(const CharacterSave& save, const MessageTable& strings);

private:
    std::vector<Entry> m_entries;
    usize m_current = 0;
    s32 m_elapsed = -120;
    bool m_spoken = false;
    bool m_awarded = false;
    bool m_gem = false;
    std::optional<WorldCamera> m_camera;
    Mat4 m_transform{1};
    const TreeInfo* m_tree = nullptr;
    TreeModel m_model;
    TreePose m_pose;
    AnimationPlayer m_player;
    TextureAnimator m_textures;
    f32 m_frames = 0;
};
} // namespace gdl::game
