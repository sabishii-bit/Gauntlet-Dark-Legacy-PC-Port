#pragma once

#include <memory>
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

#include "game/party/PlayerRuntime.h"

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
        template <class Visitor> friend void visitNativeState(Visitor& digest, const Entry& value) {
            digest.template fields<4>(value);
        }
        usize partyIndex = 0;
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
    void capturePresentation() {
        m_sim.presentationAdvanced = false;
        m_textures.advance(0);
    }
    bool active() const { return m_sim.current < m_sim.entries.size(); }
    const Entry* current() const { return active() ? &m_sim.entries[m_sim.current] : nullptr; }
    const std::optional<WorldCamera>& camera() const { return m_sim.camera; }
    const Mat4& wizardTransform() const { return m_sim.transform; }
    const TreeInfo* tree() const { return m_sim.tree; }
    const AnimationPlayer& animation() const { return m_sim.player; }
    f32 textureFrame() const { return static_cast<f32>(m_textures.frame()); }
    usize shown() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 renderAlpha = -1.0f) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, f32 width, f32 height) const;
    static std::string caption(const CharacterSave& save, const MessageTable& strings);

private:
    struct Simulation {
        std::vector<Entry> entries;
        usize current = 0;
        s32 elapsed = -120;
        bool spoken = false;
        bool awarded = false;
        bool gem = false;
        std::optional<WorldCamera> camera;
        Mat4 transform{1};
        const TreeInfo* tree = nullptr;
        TreePose pose;
        AnimationPlayer player;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationAdvanced = false;
    };

public:
    class LocalSnapshot {
        friend class TowerPromotion;
        Simulation m_state;
        TextureAnimator::Clock m_textureClock;
        const TowerPromotion* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    void appendState(NativeStateDigest& digest, const Simulation& state,
                     const TextureAnimator::Clock& clock) const;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    mutable TreeModel m_model;
    TextureAnimator m_textures;
};
} // namespace gdl::game
