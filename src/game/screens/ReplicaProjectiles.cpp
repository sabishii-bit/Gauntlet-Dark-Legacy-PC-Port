#include "game/screens/ReplicaProjectiles.h"

#include <algorithm>
#include <set>

#include "game/enemies/CombatantAssets.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
bool ProjectileResources::available(u32 id) const {
    return id != 0 && id <= 65535 && m_entries.size() < 1024 && !m_entries.contains(id);
}
bool ProjectileResources::addModel(u32 id, const TreeModel& model) {
    if (!available(id) || !model.bound() || modelId(&model) != 0) {
        return false;
    }
    auto& entry = m_entries[id];
    entry.source = &model;
    entry.model = model;
    return true;
}
bool ProjectileResources::addStreak(u32 id, const Texture& texture) {
    if (!available(id) || streakId(&texture) != 0) {
        return false;
    }
    m_entries[id].texture = &texture;
    return true;
}
bool ProjectileResources::addEffect(u32 id, const EffectTrees::Effect& prototype,
                                    RenderDevice& device) {
    if (!available(id) || prototype.archive == nullptr || prototype.tree == nullptr ||
        !prototype.model.bound() || effectId(prototype) != 0) {
        return false;
    }
    Entry entry;
    entry.archive = prototype.archive;
    entry.tree = prototype.tree;
    entry.lenders = prototype.lenders;
    entry.model = prototype.model;
    entry.textures.bind(prototype.archive->trees.textureAnimations(), prototype.archive->textures,
                        device, prototype.lenders);
    m_entries.emplace(id, std::move(entry));
    return true;
}
bool ProjectileResources::addTree(u32 id, ItemArchive& archive, std::string_view tree,
                                  RenderDevice& device, std::span<TextureSet* const> lenders) {
    const auto index = archive.trees.find(tree);
    if (!available(id) || !archive.loaded() || !index) {
        return false;
    }
    EffectTrees::Effect prototype;
    prototype.archive = &archive;
    prototype.tree = &archive.trees.tree(*index);
    prototype.lenders.assign(lenders.begin(), lenders.end());
    return prototype.model.bind(*prototype.tree, archive.models, archive.textures, device,
                                lenders) &&
           addEffect(id, prototype, device);
}
bool ProjectileResources::addCombatant(u32 firstId, CombatantAssets& stock, RenderDevice& device,
                                       std::span<TextureSet* const> lenders) {
    if (firstId == 0 || firstId > 65535 || !stock.archive.loaded()) {
        return false;
    }
    std::set<std::string> names;
    const auto collect = [&](const CritterData& data) {
        for (const auto& cue : data.sounds()) {
            if (!cue.shows() || (cue.flags & (0x0F000000U | CombatEffectDefinition::kSkin)) != 0) {
                continue; // Custom emitters/skins are not effect-tree meshes.
            }
            const auto index = stock.archive.trees.find(cue.tree);
            if (!index) {
                continue; // Shared WEAPONS effects belong to their own archive roster.
            }
            const auto& tree = stock.archive.trees.tree(*index);
            const bool mesh = std::ranges::any_of(tree.nodes, [](const auto& node) {
                return !node.object.empty() ||
                       std::ranges::any_of(node.objectFrames,
                                           [](const auto& run) { return !run.object.empty(); });
            });
            if (mesh) {
                names.insert(cue.tree);
            }
        }
    };
    collect(stock.data);
    for (const auto& child : stock.children) {
        collect(child);
    }
    if (names.size() > 65536U - firstId) {
        return false;
    }
    ProjectileResources candidate = *this;
    u32 id = firstId;
    for (const auto& name : names) {
        if (!candidate.addTree(id++, stock.archive, name, device, lenders)) {
            return false;
        }
    }
    *this = std::move(candidate);
    return true;
}
u32 ProjectileResources::modelId(const TreeModel* model) const {
    if (model != nullptr) {
        for (const auto& [id, entry] : m_entries) {
            if (entry.source == model) {
                return id;
            }
        }
    }
    return 0;
}
u32 ProjectileResources::streakId(const Texture* texture) const {
    if (texture != nullptr) {
        for (const auto& [id, entry] : m_entries) {
            if (entry.texture == texture) {
                return id;
            }
        }
    }
    return 0;
}
u32 ProjectileResources::effectId(const EffectTrees::Effect& effect) const {
    return treeId(effect.archive, effect.tree, effect.lenders);
}
u32 ProjectileResources::treeId(const ItemArchive* archive, const TreeInfo* tree,
                                std::span<TextureSet* const> lenders) const {
    for (const auto& [id, entry] : m_entries) {
        if (entry.archive == archive && entry.tree == tree && entry.tree != nullptr &&
            std::ranges::equal(entry.lenders, lenders)) {
            return id;
        }
    }
    return 0;
}
bool ProjectileResources::accepts(const ProjectileState& state) const {
    if (!state.valid()) {
        return false;
    }
    const auto found = m_entries.find(state.resource);
    if (found == m_entries.end()) {
        return false;
    }
    const auto& entry = found->second;
    if (state.source == ProjectileSource::Streak) {
        return entry.texture != nullptr && state.animation.generation == 0;
    }
    const bool effect = state.source == ProjectileSource::PlayerEffect ||
                        state.source == ProjectileSource::WorldEffect ||
                        state.source == ProjectileSource::Arrival;
    if (effect != (entry.tree != nullptr) || !entry.model.bound()) {
        return false;
    }
    if (state.animation.generation == 0) {
        return true;
    }
    return entry.tree != nullptr && state.animation.sequence < entry.tree->sequences.size() &&
           state.animation.frame <=
               static_cast<f32>(
                   std::max(0, entry.tree->sequences[state.animation.sequence].frames - 1));
}
void ProjectileResources::draw(RenderDevice& device, const ProjectileState& state, const Mat4& clip,
                               const WorldLighting& lighting, const CameraFrame& camera,
                               TreeModel::Pass pass) {
    if (!accepts(state)) {
        return;
    }
    auto& entry = m_entries.at(state.resource);
    if (state.source == ProjectileSource::Streak) {
        if (pass == TreeModel::Pass::DepthWriting || pass == TreeModel::Pass::Opaque) {
            return;
        }
        const MissileStreak streak{entry.texture, state.tint, state.forward};
        const auto batch = streak.geometry(Vec3{state.placement[3]}, state.direction, state.age,
                                           state.radius, camera);
        DrawState draw;
        draw.alphaTest = DrawState::kTranslucentAlphaTest;
        device.draw(batch, *entry.texture, clip, draw);
        return;
    }
    auto& model = entry.model;
    const auto& animation = state.animation;
    const auto occlusion = (state.flags & ProjectileState::kSolidWorld) != 0
                               ? TreeModel::Occlusion::SolidWorld
                               : TreeModel::Occlusion::Authored;
    const Mat4 placement = (state.flags & ProjectileState::kAlong) != 0
                               ? camera.along(state.placement, state.direction)
                               : state.placement;
    if (entry.tree == nullptr) {
        model.draw(device, clip, placement, lighting, {}, &camera, state.alpha, pass, occlusion);
        return;
    }
    TreePose pose;
    if (animation.generation == 0 || entry.tree->sequences[animation.sequence].frames == 0) {
        pose.rest(*entry.tree);
    } else {
        pose.evaluate(*entry.tree, animation.sequence, animation.frame, false, true);
    }
    entry.textures.apply(model, *entry.tree, animation.sequence, animation.frame,
                         state.textureFrame);
    model.setPresentationFrame(animation.sequence, animation.frame);
    model.setAppearance((state.flags & ProjectileState::kUnlit) != 0, state.tint,
                        (state.flags & ProjectileState::kDepthWrite) != 0,
                        (state.flags & ProjectileState::kAdditive) != 0);
    if (state.source == ProjectileSource::Arrival) {
        // STARTFX is a posed ring at the feet, not a camera-facing missile tree.
        model.draw(device, clip, placement, lighting, pose.matrices(), nullptr, state.alpha, pass);
        return;
    }
    const auto matrices = pose.drawMatrices(placement, camera);
    model.draw(device, clip, Mat4{1}, lighting, matrices, nullptr, state.alpha, pass, occlusion);
}

bool ProjectileCapture::append(CombatSnapshot& snapshot, const ProjectileResources& resources,
                               const PlayerMissiles& players, const EnemyMissiles& enemies,
                               const EffectTrees* worldEffects,
                               const LevelArrivalPresentation* arrival) {
    std::vector<ProjectileState> shots;
    const auto add = [&](ProjectileState state) {
        if (shots.size() >= CombatSnapshot::kMaxProjectiles || !resources.accepts(state)) {
            return false;
        }
        shots.push_back(state);
        return true;
    };
    for (usize i = 0; i < players.count(); ++i) {
        const auto& missile = players.missile(i);
        ProjectileState shot;
        shot.instance = missile.instance;
        shot.continuity = missile.continuity;
        shot.placement = PlayerMissiles::transformOf(missile);
        shot.direction = missile.velocity;
        if (missile.effect == 0 && missile.model != nullptr && missile.model->bound()) {
            shot.resource = resources.modelId(missile.model);
            shot.flags |= missile.spec->spin == 0 ? ProjectileState::kAlong : 0;
            if (!add(shot)) {
                return false;
            }
        }
        if (missile.streak.texture != nullptr) {
            shot.source = ProjectileSource::Streak;
            shot.resource = resources.streakId(missile.streak.texture);
            shot.age = missile.age;
            shot.radius = missile.spec->radius;
            shot.forward = missile.streak.forward;
            shot.tint = missile.streak.color;
            if (!add(shot)) {
                return false;
            }
        }
    }
    for (usize i = 0; i < enemies.count(); ++i) {
        const auto& missile = enemies.missile(i);
        if (missile.model == nullptr || !missile.model->bound()) {
            continue;
        }
        ProjectileState shot;
        shot.source = ProjectileSource::Enemy;
        shot.instance = missile.instance;
        shot.continuity = missile.continuity;
        shot.resource = resources.modelId(missile.model);
        shot.placement = EnemyMissiles::transformOf(missile);
        shot.flags |= ProjectileState::kSolidWorld;
        if (!add(shot)) {
            return false;
        }
    }
    const auto effects = [&](const EffectTrees& source, ProjectileSource type) {
        for (usize i = 0; i < source.count(); ++i) {
            const auto& effect = source.effect(i);
            if (effect.retiring || effect.tree == nullptr || !effect.model.bound()) {
                continue; // particle fields have their own future replication stream
            }
            ProjectileState shot;
            shot.source = type;
            shot.instance = effect.instance;
            shot.continuity = effect.continuity;
            shot.resource = resources.effectId(effect);
            shot.placement = effect.transform();
            shot.flags = (effect.depthWrite ? ProjectileState::kDepthWrite : 0) |
                         (effect.unlit ? ProjectileState::kUnlit : 0) |
                         (effect.additive ? ProjectileState::kAdditive : 0);
            if (effect.flightDirection) {
                shot.flags |= ProjectileState::kAlong;
                shot.direction = *effect.flightDirection;
            }
            shot.tint = effect.tint;
            shot.alpha = effect.opacity();
            shot.textureFrame = source.textureFrame(effect);
            if (!effect.tree->sequences.empty() && effect.player.playing()) {
                shot.animation = {0, effect.player.sequence(), effect.player.generation(),
                                  effect.player.frame(), 1};
            }
            if (!add(shot)) {
                return false;
            }
        }
        return true;
    };
    if (!effects(players.visuals(), ProjectileSource::PlayerEffect) ||
        (worldEffects != nullptr && !effects(*worldEffects, ProjectileSource::WorldEffect))) {
        return false;
    }
    if (arrival != nullptr) {
        for (usize i = 0; i < arrival->effectCount(); ++i) {
            if (const auto view = arrival->effectPresentation(i)) {
                ProjectileState state;
                state.source = ProjectileSource::Arrival;
                state.instance = i + 1;
                state.resource = resources.treeId(view->archive, view->tree);
                state.placement = glm::translate(Mat4{1}, view->position);
                state.animation = {0, view->sequence, view->generation, view->frame, 1};
                state.textureFrame = view->textureClock;
                if (!add(state)) {
                    return false;
                }
            }
        }
    }
    std::ranges::sort(shots, {}, &ProjectileState::key);
    auto candidate = snapshot;
    candidate.projectiles = std::move(shots);
    if (!candidate.valid()) {
        return false;
    }
    snapshot = std::move(candidate);
    return true;
}
bool ReplicaProjectiles::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_shots.clear();
    return true;
}
void ReplicaProjectiles::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_shots.clear();
}
bool ReplicaProjectiles::show(const CombatSnapshot& snapshot,
                              const ProjectileResources& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) ||
        !std::ranges::all_of(snapshot.projectiles,
                             [&](const auto& shot) { return resources.accepts(shot); })) {
        return false;
    }
    m_tick = snapshot.motion.tick;
    m_shots = snapshot.projectiles;
    return true;
}
void ReplicaProjectiles::draw(RenderDevice& device, ProjectileResources& resources,
                              const Mat4& clip, const WorldLighting& lighting,
                              const CameraFrame& camera, TreeModel::Pass pass) const {
    for (const auto& shot : m_shots) {
        resources.draw(device, shot, clip, lighting, camera, pass);
    }
}
} // namespace gdl::game
