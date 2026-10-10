#include "game/screens/ReplicaCompanions.h"

#include <algorithm>

namespace gdl::game {
void CompanionResources::bindEarned(const PlayerFigure& figure) {
    m_earned = {};
    m_earnedForm = 0;
    const auto shown = figure.companionVisuals(Mat4{1}, 1)[0];
    if (shown) {
        m_earned = {shown->tree, *shown->model, *shown->textures};
        m_earnedForm = shown->form;
    }
}
bool CompanionResources::bindPowerups(RenderDevice& device, ItemArchive& powerups,
                                      ItemArchive* weapons) {
    std::array<Entry, 7> ready;
    for (usize i = 0; i < ready.size(); ++i) {
        const auto kind = static_cast<PowerupCompanion::Kind>(i + 1);
        ItemArchive* archive = PowerupCompanion::fromWeapons(kind) ? weapons : &powerups;
        if (archive == nullptr || !archive->loaded()) {
            return false;
        }
        const auto tree = archive->trees.find(PowerupCompanion::treeOf(kind));
        if (!tree) {
            return false;
        }
        auto& resource = ready[i];
        resource.tree = &archive->trees.tree(*tree);
        if (resource.tree->sequences.empty() ||
            !resource.model.bind(*resource.tree, archive->models, archive->textures, device)) {
            return false;
        }
        resource.textures.bind(archive->trees.textureAnimations(), archive->textures, device);
    }
    m_powerups = std::move(ready);
    return true;
}
const CompanionResources::Entry* CompanionResources::entry(usize slot, u32 form) const {
    if (slot == 0) {
        return form != 0 && form == m_earnedForm ? &m_earned : nullptr;
    }
    return slot == 1 && form >= 1 && form <= m_powerups.size() ? &m_powerups[form - 1] : nullptr;
}
bool CompanionResources::accepts(usize slot, const CompanionState& state) const {
    const auto* resource = entry(slot, state.form);
    return state.valid(slot) && resource != nullptr && resource->model.bound() &&
           resource->tree != nullptr &&
           state.animation.sequence < resource->tree->sequences.size() &&
           state.animation.frame <=
               static_cast<f32>(
                   std::max(0, resource->tree->sequences[state.animation.sequence].frames - 1));
}
void CompanionResources::draw(RenderDevice& device, usize slot, const CompanionState& state,
                              const Mat4& clip, const WorldLighting& lighting,
                              const CameraFrame& camera, TreeModel::Pass pass) const {
    if (!accepts(slot, state)) {
        return;
    }
    const auto& resource = *entry(slot, state.form);
    const auto& animation = state.animation;
    TreePose pose;
    pose.evaluate(*resource.tree, animation.sequence, animation.frame, false, true);
    resource.model.setPresentationFrame(animation.sequence, animation.frame);
    resource.textures.apply(resource.model, *resource.tree, animation.sequence, animation.frame,
                            state.textureClock - static_cast<f32>(resource.textures.frame()));
    resource.model.draw(device, clip, state.placement, lighting, pose.matrices(), &camera,
                        state.alpha, pass);
}
} // namespace gdl::game
