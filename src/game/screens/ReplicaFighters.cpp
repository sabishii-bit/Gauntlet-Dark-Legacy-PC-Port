#include "game/screens/ReplicaFighters.h"

#include <algorithm>

namespace gdl::game {
bool FighterResources::bind(const Critters& critters, const Bosses& bosses) {
    auto stocks = critters.resources();
    for (const auto* stock : bosses.resources()) {
        stocks.push_back(stock);
    }
    return bind(stocks);
}
bool FighterResources::bind(std::span<const CombatantAssets* const> stocks) {
    std::vector<const CombatantAssets*> ordered(stocks.begin(), stocks.end());
    if (std::ranges::any_of(ordered, [](const auto* stock) {
            return stock == nullptr || stock->tree == nullptr;
        })) {
        return false;
    }
    std::ranges::sort(ordered, {}, [](const auto* stock) { return stock->definition.name; });
    FighterResources next;
    for (usize s = 0; s < ordered.size(); ++s) {
        const auto* stock = ordered[s];
        if (s > 0 && ordered[s - 1]->definition.name == stock->definition.name) {
            return false;
        }
        std::vector<const Texture*> skins;
        for (const auto& [name, frames] : stock->skins) {
            skins.insert(skins.end(), frames.begin(), frames.end());
        }
        if (skins.size() > 65535) {
            return false;
        }
        const auto add = [&](const TreeModel& model, const TreeInfo* tree, bool textured) {
            if (model.bound()) {
                next.m_entries.push_back(
                    {stock, &model, tree, textured, model, stock->textures, skins});
            }
        };
        add(stock->body, stock->tree, true);
        for (const auto& attachment : stock->attachments) {
            add(attachment.model, attachment.tree, true);
        }
        for (const auto& [name, model] : stock->brokenModels) {
            add(model, nullptr, false);
        }
        for (const auto& [name, model] : stock->replacementModels) {
            add(model, nullptr, false);
        }
        add(stock->meter, stock->meterTree, false);
    }
    if (next.m_entries.size() > 65535) {
        return false;
    }
    *this = std::move(next);
    return true;
}
std::optional<FighterMeshState> FighterResources::capture(const CombatantVisual& visual, u32 actor,
                                                          u64 incarnation) const {
    const auto found = std::ranges::find_if(m_entries, [&](const auto& entry) {
        return entry.stock == visual.stock && entry.source == visual.model;
    });
    if (found == m_entries.end() ||
        visual.textureTree != (found->textured ? found->tree : nullptr)) {
        return std::nullopt;
    }
    FighterMeshState state;
    state.actor = actor;
    state.incarnation = incarnation;
    state.part = visual.part;
    state.resource = static_cast<u32>(found - m_entries.begin() + 1);
    state.placement = visual.placement;
    state.sequence = visual.sequence;
    state.frame = visual.frame;
    state.textureClock = visual.textureClock;
    state.alpha = visual.alpha;
    state.tint = visual.tint;
    state.flags = (visual.unlit ? FighterMeshState::kUnlit : 0U) |
                  (visual.flash ? FighterMeshState::kFlash : 0U) |
                  (visual.frozen ? FighterMeshState::kFrozen : 0U);
    if (visual.skin != nullptr) {
        const auto skin = std::ranges::find(found->skins, visual.skin);
        if (skin == found->skins.end()) {
            return std::nullopt;
        }
        state.skin = static_cast<u32>(skin - found->skins.begin() + 1);
    }
    for (const auto& node : visual.nodes) {
        state.nodes.push_back(
            {node.transform, node.generation, node.sequence, node.frame, node.alpha, node.flash});
    }
    return accepts(state) ? std::optional{std::move(state)} : std::nullopt;
}
bool FighterResources::accepts(const FighterMeshState& state) const {
    if (!state.valid() || state.resource > m_entries.size()) {
        return false;
    }
    const auto& entry = m_entries[state.resource - 1];
    if (state.skin > entry.skins.size() ||
        state.nodes.size() != (entry.tree != nullptr ? entry.tree->nodes.size() : 0)) {
        return false;
    }
    const auto cursor = [&](u32 sequence, f32 frame) {
        return entry.tree == nullptr || entry.tree->sequences.empty() || !entry.textured
                   ? sequence == 0 && frame == 0
                   : sequence < entry.tree->sequences.size() &&
                         frame <= static_cast<f32>(
                                      std::max(0, entry.tree->sequences[sequence].frames - 1));
    };
    return cursor(state.sequence, state.frame) &&
           std::ranges::all_of(state.nodes,
                               [&](const auto& node) { return cursor(node.sequence, node.frame); });
}
void FighterResources::draw(RenderDevice& device, const FighterMeshState& state, const Mat4& clip,
                            const WorldLighting& lighting, const Texture* flash,
                            const Texture* frozen, TreeModel::Pass pass) {
    if (!accepts(state)) {
        return;
    }
    auto& entry = m_entries[state.resource - 1];
    auto& model = entry.model;
    model.resetTextures();
    if (entry.textured) {
        entry.textures.apply(model, *entry.tree, state.sequence, state.frame,
                             state.textureClock - static_cast<f32>(entry.textures.frame()));
    }
    const bool flashing = (state.flags & FighterMeshState::kFlash) != 0 && flash != nullptr;
    model.setAppearance(flashing || (state.flags & FighterMeshState::kUnlit) != 0,
                        flashing && entry.textured ? Color::white() : state.tint);
    const Texture* skin = state.skin != 0 ? entry.skins[state.skin - 1] : nullptr;
    if (flashing) {
        skin = flash;
    }
    if ((state.flags & FighterMeshState::kFrozen) != 0 && frozen != nullptr) {
        skin = frozen;
    }
    model.setMaskedTexture(skin);
    std::vector<Mat4> matrices;
    matrices.reserve(state.nodes.size());
    for (usize i = 0; i < state.nodes.size(); ++i) {
        const auto& node = state.nodes[i];
        matrices.push_back(node.transform);
        model.setMeshPresentationFrame(i, node.sequence, node.frame);
        if (node.alpha != 1) {
            model.setMeshAlpha(i, node.alpha);
        }
        if (node.flash && flash != nullptr) {
            model.setMeshMaskedTexture(i, flash);
        }
    }
    model.draw(device, clip, state.placement, lighting, matrices, nullptr, state.alpha, pass);
}
bool FighterCapture::append(CombatSnapshot& snapshot, const FighterResources& resources,
                            const Critters& critters, const Bosses& bosses) {
    std::vector<FighterMeshState> meshes;
    const auto camera = CameraFrame::of(snapshot.motion.camera);
    const auto add = [&](const Combatant& fighter, u32 actor, const CameraFrame* frame) {
        for (const auto& visual : fighter.visuals(frame)) {
            auto state = resources.capture(visual, actor, fighter.incarnation());
            if (!state) {
                return false;
            }
            meshes.push_back(std::move(*state));
        }
        return true;
    };
    if (!add(bosses.fighter(), 0, nullptr)) {
        return false;
    }
    for (usize i = 0; i < critters.fighters().size(); ++i) {
        if (!add(critters.fighters()[i], static_cast<u32>(i + 1), &camera)) {
            return false;
        }
    }
    std::ranges::sort(meshes, {}, &FighterMeshState::key);
    if (!snapshot.valid() || !FighterPacket::valid(meshes)) {
        return false;
    }
    snapshot.fighters = std::move(meshes);
    return true;
}
bool ReplicaFighters::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    clear();
    m_epoch = epoch;
    return true;
}
void ReplicaFighters::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_meshes.clear();
}
bool ReplicaFighters::show(const CombatSnapshot& snapshot, const FighterResources& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) ||
        !std::ranges::all_of(snapshot.fighters,
                             [&](const auto& state) { return resources.accepts(state); })) {
        return false;
    }
    m_meshes = snapshot.fighters;
    m_tick = snapshot.motion.tick;
    return true;
}
void ReplicaFighters::draw(RenderDevice& device, FighterResources& resources, const Mat4& clip,
                           const WorldLighting& lighting, const Texture* flash,
                           const Texture* frozen, TreeModel::Pass pass) const {
    for (const auto& state : m_meshes) {
        resources.draw(device, state, clip, lighting, flash, frozen, pass);
    }
}
} // namespace gdl::game
