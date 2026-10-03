#pragma once

#include <random>

#include "engine/core/Types.h"

#include "game/players/MikeyDecoy.h"
#include "game/world/EffectTrees.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {
/** The dropped decoy's animated POWERUPS tree and its brief arrival sparkles. */
class MikeyFigure {
public:
    void update(RenderDevice& device, ItemArchive& powerups, MikeyDecoy& decoy, f32 seconds,
                EffectTrees& effects);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera) const;
    bool shown() const { return m_shown && m_figure.hasFigure(); }
    void clear();

private:
    ItemFigure m_figure;
    u32 m_generation = 0;
    bool m_shown = false;
    std::mt19937 m_random{0};
};
} // namespace gdl::game
