#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"

#include "game/world/TowerCamera.h"

namespace gdl::game {

/** Paired TRANS items move individual players within a level, not through its exits. */
class LevelTransporters {
public:
    static constexpr s32 kItemType = 11;
    struct Pad {
        s32 instance = -1;
        s32 id = 0;
        s32 destinationId = 0;
        std::optional<usize> destination;
        Vec3 position{0.0f};
        f32 radius = 0.0f;
        f32 height = 0.0f;
        Mat4 transform{1.0f};
        TreeModel model;
        TreePose pose;
        AnimationPlayer animation;
    };

    /** Both archives must outlive the pads; level-specific art takes precedence. */
    void bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items, s32 players,
              ItemArchive* realmItems = nullptr);
    void clear();
    void animate(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    usize size() const { return m_pads.size(); }
    const Pad& pad(usize index) const { return m_pads[index]; }
    std::optional<usize> contact(const Vec3& feet, f32 radius, f32 height) const;
    /** Reject missing links, offscreen destinations and landings with no floor. */
    std::optional<Vec3> landing(usize source, f32 radius, const WorldCollision& collision,
                                const WorldCamera& camera, const CameraView& view) const;
    static bool visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                        const CameraView& view);
    static std::string_view soundForRealm(s32 realm);

private:
    std::vector<Pad> m_pads;
    const TreeInfo* m_tree = nullptr;
    TextureAnimator m_textures;
    f32 m_frames = 0.0f;
};

} // namespace gdl::game
