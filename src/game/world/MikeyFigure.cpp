#include "game/world/MikeyFigure.h"

#include <array>
#include <string_view>

namespace gdl::game {
void MikeyFigure::update(RenderDevice& device, ItemArchive& powerups, MikeyDecoy& decoy,
                         f32 seconds, EffectTrees& effects) {
    if (!decoy.shown()) {
        clear();
        return;
    }
    if (!m_shown || m_generation != decoy.generation()) {
        ItemInstance instance;
        instance.position = decoy.position();
        m_figure.place(device, powerups, "MIKEYPUP_ON", instance, nullptr);
        m_generation = decoy.generation();
        m_shown = true;
    } else {
        m_figure.update(seconds);
    }
    constexpr std::array<std::string_view, 4> kGems{"GETGEMORANGE", "GETGEMRED", "GETGEMPURPLE",
                                                    "GETGEMBLUE"};
    for (s32 count = decoy.takeSparkles(); count > 0; --count) {
        effects.start(device, powerups, kGems[m_random() % kGems.size()], decoy.position());
    }
}

void MikeyFigure::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const CameraFrame* camera) const {
    if (m_shown) {
        m_figure.draw(device, clip, lighting, 1, 1, camera);
    }
}

void MikeyFigure::clear() {
    m_shown = false;
    m_figure = {};
}
} // namespace gdl::game
