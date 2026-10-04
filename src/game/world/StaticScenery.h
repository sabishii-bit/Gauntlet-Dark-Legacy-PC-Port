#pragma once

#include <memory>
#include <vector>

#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"
#include "game/world/ItemSupport.h"

namespace gdl::game {
/** Ordinary type-10/subtype-0 item scenery, including the level's exit sign.
 * These are solid placed items, not exit triggers or breakable scenery. */
class StaticScenery {
public:
    struct Prop {
        s32 instance = -1;
        s32 minPlayers = 1;
        bool shown = true;
        ItemFigure figure;
        ItemSupport support;
        Obstacle box;
    };
    void bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, ItemArchive* realmItems = nullptr);
    void clear();
    void setPlayerCount(s32 players);
    void capturePresentation();
    void update(f32 seconds);
    void syncFloors();
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera, TreeModel::Pass pass, f32 presentationAlpha) const;
    usize size() const { return m_props.size(); }
    const Prop& prop(usize index) const { return *m_props[index]; }

private:
    const WorldCollision* m_collision = nullptr;
    std::vector<std::unique_ptr<Prop>> m_props;
};
} // namespace gdl::game
