#include "game/screens/ReplicaFixtures.h"

#include <algorithm>

namespace gdl::game {
bool FixtureResources::bind(RenderDevice& device, std::span<ItemArchive* const> archives,
                            const Generators& generators, const SafeRocks& rocks,
                            const CritterStatues* statues) {
    FixtureResources next;
    std::vector<ItemArchive*> sources(archives.begin(), archives.end());
    if (statues != nullptr) {
        for (usize i = 0; i < statues->count(); ++i) {
            sources.push_back(&statues->archive(i));
        }
    }
    std::vector<ItemArchive*> seen;
    for (auto* archive : sources) {
        if (archive == nullptr || !archive->loaded()) {
            return false;
        }
        if (std::ranges::find(seen, archive) != seen.end()) {
            continue;
        }
        seen.push_back(archive);
        TextureAnimator textures;
        textures.bind(archive->trees.textureAnimations(), archive->textures, device);
        for (usize index = 0; index < archive->trees.size(); ++index) {
            Entry entry;
            entry.archive = archive;
            entry.tree = &archive->trees.tree(static_cast<u32>(index));
            entry.model.bind(*entry.tree, archive->models, archive->textures, device);
            entry.textures = textures;
            next.m_entries.push_back(std::move(entry));
        }
        for (const auto object :
             {Rubble::kItem, Rubble::kChest, Rubble::kSilverChest, Rubble::kBlownBarrel,
              Rubble::kGasBarrel, TowerAccess::kOffFigure}) {
            if (!archive->models.find(object)) {
                continue;
            }
            Entry entry;
            entry.archive = archive;
            entry.object = object;
            entry.staticTree = std::make_unique<TreeInfo>();
            TreeNodeInfo node;
            node.object = object;
            entry.staticTree->nodes.push_back(node);
            entry.tree = entry.staticTree.get();
            entry.model.bind(*entry.tree, archive->models, archive->textures, device);
            next.m_entries.push_back(std::move(entry));
        }
    }
    for (const auto& body : generators.bodyResources()) {
        Entry entry;
        entry.tree = body.tree;
        entry.model = *body.model;
        entry.textures = *body.textures;
        entry.generator = std::pair{body.kind, body.state};
        next.m_entries.push_back(std::move(entry));
    }
    for (const auto& body : rocks.bodyResources()) {
        Entry entry;
        entry.staticTree = std::make_unique<TreeInfo>();
        entry.tree = entry.staticTree.get();
        entry.model = *body.model;
        entry.textures = *body.textures;
        entry.rock = std::pair{body.index, body.tier};
        next.m_entries.push_back(std::move(entry));
    }
    if (next.m_entries.size() > 65535) {
        return false;
    }
    *this = std::move(next);
    return true;
}
u32 FixtureResources::id(const ItemFigure::Presentation& figure) const {
    for (usize index = 0; index < m_entries.size(); ++index) {
        const auto& entry = m_entries[index];
        if (entry.tree == figure.tree && entry.archive == figure.archive) {
            return static_cast<u32>(index + 1);
        }
    }
    return 0;
}
u32 FixtureResources::generatorId(s32 kind, s32 state) const {
    for (usize index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].generator == std::pair{kind, state}) {
            return static_cast<u32>(index + 1);
        }
    }
    return 0;
}
u32 FixtureResources::portalId(const ExitPortals::Portal& portal) const {
    if (portal.shut) {
        return objectId(portal.archive, TowerAccess::kOffFigure);
    }
    for (usize index = 0; index < m_entries.size(); ++index) {
        const auto& entry = m_entries[index];
        if (entry.archive == portal.archive && entry.tree->name == ExitPortals::kFigure) {
            return static_cast<u32>(index + 1);
        }
    }
    return 0;
}
bool FixtureResources::accepts(const FixtureState& fixture) const {
    if (!fixture.valid() || fixture.resource > m_entries.size()) {
        return false;
    }
    const auto& entry = m_entries[fixture.resource - 1];
    const auto cursor = [&](u32 sequence, f32 frame) {
        return entry.tree->sequences.empty()
                   ? sequence == 0 && frame == 0
                   : sequence < entry.tree->sequences.size() &&
                         frame <= static_cast<f32>(
                                      std::max(0, entry.tree->sequences[sequence].frames - 1));
    };
    return entry.model.bound() &&
           (fixture.pose.generation == 0 || cursor(fixture.pose.sequence, fixture.pose.frame)) &&
           cursor(fixture.meshSequence, fixture.meshFrame) &&
           cursor(fixture.textureSequence, fixture.textureFrame);
}
u32 FixtureResources::rockId(usize index, s32 tier) const {
    for (usize i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rock == std::pair{index, tier}) {
            return static_cast<u32>(i + 1);
        }
    }
    return 0;
}
u32 FixtureResources::objectId(const ItemArchive* archive, std::string_view object) const {
    for (usize i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].archive == archive && m_entries[i].object == object) {
            return static_cast<u32>(i + 1);
        }
    }
    return 0;
}
void FixtureResources::draw(RenderDevice& device, const FixtureState& fixture, const Mat4& clip,
                            const WorldLighting& lighting, const CameraFrame& camera,
                            TreeModel::Pass pass) {
    if (!accepts(fixture)) {
        return;
    }
    auto& entry = m_entries[fixture.resource - 1];
    TreePose pose;
    if (fixture.pose.generation == 0) {
        pose.rest(*entry.tree);
    } else {
        pose.evaluate(*entry.tree, fixture.pose.sequence, fixture.pose.frame, false, true);
    }
    entry.model.resetTextures();
    // Ordinary exits use posed/object animation only. Secret icons use the
    // ItemFigure texture clock like other fixtures; do not invent one for exits.
    if (fixture.source != FixtureSource::Portal) {
        entry.textures.apply(entry.model, *entry.tree, fixture.textureSequence,
                             fixture.textureFrame,
                             fixture.textureClock - static_cast<f32>(entry.textures.frame()));
    }
    entry.model.setPresentationFrame(fixture.meshSequence, fixture.meshFrame);
    entry.model.draw(device, clip, fixture.placement, lighting, pose.matrices(),
                     fixture.cameraFacing ? &camera : nullptr, fixture.alpha, pass);
}
bool FixtureCapture::append(CombatSnapshot& snapshot, const FixtureResources& resources,
                            const Sources& sources) {
    const auto& [chests, gates, switches, generators, barrels, traps, rocks, rubble, statues,
                 portals] = sources;
    std::vector<FixtureState> visible;
    const auto add = [&](const FixtureState& state) {
        if (visible.size() >= CombatSnapshot::kMaxFixtures || !resources.accepts(state)) {
            return false;
        }
        visible.push_back(state);
        return true;
    };
    const auto figure = [&](FixtureSource source, u64 index, const ItemFigure& item, bool camera,
                            f32 alpha = 1, f32 scale = 1) {
        const auto view = item.presentation();
        if (!view.drawable) {
            return true;
        }
        FixtureState state;
        state.source = source;
        state.instance = index + 1;
        state.resource = resources.id(view);
        state.continuity = view.continuity;
        state.placement = glm::scale(item.transform(), Vec3{scale});
        state.pose = {0, view.poseSequence, view.poseGeneration, view.poseFrame, 1};
        state.meshSequence = view.meshSequence;
        state.meshFrame = view.meshFrame;
        state.textureSequence = view.textureSequence;
        state.textureFrame = view.textureFrame;
        state.textureClock = view.textureClock;
        state.alpha = alpha;
        state.cameraFacing = camera;
        return add(state);
    };
    for (usize i = 0; i < chests.size(); ++i) {
        const auto& chest = chests.chest(i);
        if (chest.shown && !chest.gone &&
            (!figure(FixtureSource::Chest, i, chest.figure, true,
                     chest.revealed ? 63.0f / 255 : 1) ||
             (chest.revealed &&
              !figure(FixtureSource::ChestPreview, i, chest.preview, true, 1, 0.65f)))) {
            return false;
        }
    }
    for (usize i = 0; i < gates.size(); ++i) {
        const auto& gate = gates.gate(i);
        if (gate.shown && !figure(FixtureSource::Gate, i, gate.figure, false)) {
            return false;
        }
    }
    for (usize i = 0; i < switches.size(); ++i) {
        const auto* item = switches.figure(i);
        if (switches.trigger(i).enabled && item != nullptr &&
            !figure(FixtureSource::Switch, i, *item, false)) {
            return false;
        }
    }
    for (const auto& generator : generators.presentation()) {
        if (generator.bossFigure != nullptr) {
            if (!figure(FixtureSource::Generator, generator.index, *generator.bossFigure, false)) {
                return false;
            }
        } else {
            FixtureState state;
            state.source = FixtureSource::Generator;
            state.instance = generator.index + 1;
            state.resource = resources.generatorId(generator.kind, generator.state);
            state.placement = generator.placement;
            state.textureClock = generator.textureClock;
            if (!add(state)) {
                return false;
            }
        }
    }
    for (usize i = 0; i < barrels.size(); ++i) {
        const auto& barrel = barrels.barrel(i);
        if (barrel.shown && !barrel.gone &&
            !figure(FixtureSource::Barrel, i, barrel.figure, false, barrels.opacityOf(i))) {
            return false;
        }
    }
    for (usize i = 0; i < traps.size(); ++i) {
        const auto& trap = traps.trap(i);
        if (trap.shown && !figure(FixtureSource::Trap, i, trap.figure, true)) {
            return false;
        }
    }
    for (usize i = 0; i < rocks.size(); ++i) {
        const auto& rock = rocks.rock(i);
        if (rock.shown && !rock.dormant && rock.models[static_cast<usize>(rock.tier)].bound()) {
            FixtureState state;
            state.source = FixtureSource::Rock;
            state.instance = i + 1;
            state.resource = resources.rockId(i, rock.tier);
            state.placement = rock.placement;
            state.textureClock = rocks.textureClock();
            if (!add(state)) {
                return false;
            }
        }
    }
    for (const auto& piece : rubble.presentation()) {
        FixtureState state;
        state.source = FixtureSource::Rubble;
        state.instance = piece.index + 1;
        state.resource = resources.objectId(piece.archive, piece.object);
        state.placement = piece.placement;
        if (!add(state)) {
            return false;
        }
    }
    if (statues != nullptr) {
        for (usize i = 0; i < statues->count(); ++i) {
            if (!figure(FixtureSource::Statue, statues->instanceOf(i) - 1, statues->figure(i),
                        true)) {
                return false;
            }
        }
    }
    if (portals != nullptr) {
        for (usize i = 0; i < portals->size(); ++i) {
            const auto& portal = portals->portal(i);
            if (portal.secret) {
                if (!portal.consumed &&
                    !figure(FixtureSource::SecretPortal, i, portal.icon, true)) {
                    return false;
                }
                continue;
            }
            if (!portal.model.bound()) {
                continue;
            }
            FixtureState state;
            state.source = FixtureSource::Portal;
            state.instance = i + 1;
            state.resource = resources.portalId(portal);
            state.placement = portal.transform;
            state.alpha = portal.alpha;
            state.cameraFacing = true;
            if (!portal.shut && portal.player.playing()) {
                state.pose = {0, portal.player.sequence(), portal.player.generation(),
                              portal.player.frame(), 1};
                state.meshSequence = state.textureSequence = portal.player.sequence();
                state.meshFrame = state.textureFrame = portal.player.frame();
            }
            if (!add(state)) {
                return false;
            }
        }
    }
    std::ranges::sort(visible, {}, &FixtureState::key);
    auto candidate = snapshot;
    candidate.fixtures = std::move(visible);
    if (!candidate.valid()) {
        return false;
    }
    snapshot = std::move(candidate);
    return true;
}
bool ReplicaFixtures::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_fixtures.clear();
    return true;
}
void ReplicaFixtures::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_fixtures.clear();
}
bool ReplicaFixtures::show(const CombatSnapshot& snapshot, const FixtureResources& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) ||
        !std::ranges::all_of(snapshot.fixtures,
                             [&](const auto& state) { return resources.accepts(state); })) {
        return false;
    }
    m_tick = snapshot.motion.tick;
    m_fixtures = snapshot.fixtures;
    return true;
}
void ReplicaFixtures::draw(RenderDevice& device, FixtureResources& resources, const Mat4& clip,
                           const WorldLighting& lighting, const CameraFrame& camera,
                           TreeModel::Pass pass) const {
    for (const auto& fixture : m_fixtures) {
        resources.draw(device, fixture, clip, lighting, camera, pass);
    }
}
} // namespace gdl::game
