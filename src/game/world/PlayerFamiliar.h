#pragma once

#include <string_view>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {
/** Animated companion borrowing its permanent-class or temporary-powerup archive. */
class PlayerFamiliar {
public:
    static s32 tierFor(s32 level) {
        if (level >= 80) {
            return 2;
        }
        return level >= 30 ? 1 : 0;
    }
    bool bind(RenderDevice& device, ItemArchive& archive, s32 level, const Vec3& offset);
    bool bindPhoenix(RenderDevice& device, ItemArchive& archive);
    bool bound() const { return m_tree != nullptr; }
    void update(f32 seconds, bool attack);
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
              const WorldLighting& lighting, f32 alpha, const CameraFrame* camera = nullptr) const;
    s32 tier() const { return m_tree != nullptr ? m_tier : 0; }

private:
    bool bindTree(RenderDevice& device, ItemArchive& archive, std::string_view name,
                  const Vec3& offset);
    const TreeInfo* m_tree = nullptr;
    TreeModel m_model;
    TreePose m_pose;
    AnimationPlayer m_player;
    TextureAnimator m_textures;
    Vec3 m_offset{0};
    f32 m_frames = 0;
    s32 m_tier = 0;
};
} // namespace gdl::game
