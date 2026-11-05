#pragma once

#include <memory>
#include <string_view>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCamera.h"

#include "game/party/CompanionVisual.h"

namespace gdl::game {
/** The familiar a character earns at thirty and eighty, animated from its class archive. */
class PlayerFamiliar {
    struct State {
        TreePose pose;
        TreePose previousPose;
        AnimationPlayer player;
        TextureAnimator::Clock textures;
        Vec3 offset{0};
        u64 previousGeneration = 0;
        f32 previousFrame = 0;
        bool presentationAdvanced = false;
        s32 tier = 0;
    };

public:
    /** Local companion presentation history, separate from the loaded artwork.
     * The familiar does not decide when missiles are released; PlayerAnimator does. */
    class LocalSnapshot {
        friend class PlayerFamiliar;
        State m_state;
        std::weak_ptr<const u8> m_owner;
        u64 m_bindingRevision = 0;
    };
    void saveLocal(LocalSnapshot& state) const;
    bool canRestoreLocal(const LocalSnapshot& state) const;
    bool restoreLocal(const LocalSnapshot& state);
    /** Actual captured values and immutable binding, never a draw-sampled clock. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static s32 tierFor(s32 level) {
        if (level >= 80) {
            return 2;
        }
        return level >= 30 ? 1 : 0;
    }
    bool bind(RenderDevice& device, ItemArchive& archive, s32 level, const Vec3& offset,
              std::span<TextureSet* const> lenders = {});
    bool bound() const { return m_tree != nullptr; }
    void update(f32 seconds, bool attack);
    void capturePresentation() {
        m_state.presentationAdvanced = false;
        m_state.textures.advance = 0;
    }
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
              const WorldLighting& lighting, f32 alpha, const CameraFrame* camera = nullptr,
              f32 renderAlpha = 1.0f, TreeModel::Pass pass = TreeModel::Pass::All) const;
    s32 tier() const { return m_tree != nullptr ? m_state.tier : 0; }
    std::optional<CompanionVisual> visual(const Mat4& body, f32 alpha) const;

private:
    bool appendState(NativeStateDigest& digest, const State& state) const;
    bool bindTree(RenderDevice& device, ItemArchive& archive, std::string_view name,
                  const Vec3& offset, std::span<TextureSet* const> lenders);
    const TreeInfo* m_tree = nullptr;
    mutable TreeModel m_model; ///< render-only mesh sampling never advances animation clocks
    State m_state;
    mutable TreePose m_drawPose;
    mutable TextureAnimator m_textures;
    u64 m_bindingRevision = 0;
    std::shared_ptr<const u8> m_localOwner = std::make_shared<const u8>(0);
};
} // namespace gdl::game
