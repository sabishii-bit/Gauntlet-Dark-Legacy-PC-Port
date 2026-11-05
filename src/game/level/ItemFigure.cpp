#include "game/level/ItemFigure.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/effects/ThermalTextures.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {

void visitNativeState(NativeStateDigest& digest, const ItemFigure& figure) {
    const auto& [resources, textureClock, tree, archive, poseSequence, poseFrame, poseGeneration,
                 continuity, pose, previousTransform, previousFrame, previousGeneration, captured,
                 advanced, player, particles, textureSequence, textureFrame, gateParticles,
                 position, yaw, transform, placement, index, loop, holdPose] = figure.m_sim;
    // The model/cache behind resources is derived from these immutable asset
    // bindings and the value clocks. Its current draw sample is not tick state.
    digest.flag(resources != nullptr);
    digest.resource(tree);
    digest.resource(archive);
    digest.fields<3>(textureClock);
    digest.values(poseSequence, poseFrame, poseGeneration, continuity, pose, previousTransform,
                  previousFrame, previousGeneration, captured, advanced, player, particles,
                  textureSequence, textureFrame, gateParticles, position, yaw, transform, placement,
                  index, loop, holdPose);
}

namespace {

constexpr f32 kTicksPerSecond = 60.0f;
constexpr s32 kExactPlayersMark = 10;
constexpr f32 kPresentationCutDistance = 32.0f;

Mat4 blendPlacement(const Mat4& previous, const Mat4& current, f32 blend) {
    Mat3 from{previous};
    Mat3 to{current};
    Vec3 fromScale{0};
    Vec3 toScale{0};
    for (s32 axis = 0; axis < 3; ++axis) {
        fromScale[axis] = glm::length(from[axis]);
        toScale[axis] = glm::length(to[axis]);
        if (fromScale[axis] < 1e-6f || toScale[axis] < 1e-6f) {
            return current;
        }
        from[axis] /= fromScale[axis];
        to[axis] /= toScale[axis];
    }
    const auto orthogonal = [](const Mat3& basis) {
        return std::abs(glm::determinant(basis) - 1.0f) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[1])) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[2])) < 1e-3f &&
               std::abs(glm::dot(basis[1], basis[2])) < 1e-3f;
    };
    Mat4 result = current;
    if (orthogonal(from) && orthogonal(to)) {
        result = glm::mat4_cast(portable::slerp(glm::quat_cast(from), glm::quat_cast(to), blend));
        result = glm::scale(result, glm::mix(fromScale, toScale, blend));
    }
    result[3] = glm::mix(previous[3], current[3], blend);
    return result;
}

/** `position` in the box's own space, about its centre. */
Vec2 localOf(const Obstacle& box, const Vec3& position) {
    const f32 dx = position.x - box.centre.x;
    const f32 dz = position.z - box.centre.z;
    const f32 c = portable::cos(box.yaw);
    const f32 s = portable::sin(box.yaw);
    return Vec2{dx * c - dz * s, dx * s + dz * c};
}

bool levelWith(const Obstacle& box, const Vec3& position) {
    return position.y <= box.centre.y + box.height && position.y + box.height >= box.centre.y;
}

} // namespace

ItemFigure::ItemFigure(const ItemFigure& other)
    : m_bindings(other.m_bindings), m_sim(other.m_sim) {}

ItemFigure& ItemFigure::operator=(const ItemFigure& other) {
    if (this != &other) {
        m_sim = other.m_sim;
        m_bindings = other.m_bindings;
        m_presentationPose = {};
    }
    return *this;
}

bool ItemFigure::equalLocal(const ItemFigure& other) const {
    const auto values = [](const Simulation& s) {
        return std::tie(s.tree, s.archive, s.textureClock, s.poseSequence, s.poseFrame,
                        s.poseGeneration, s.continuity, s.previousTransform, s.previousFrame,
                        s.previousGeneration, s.presentationCaptured, s.presentationAdvanced,
                        s.textureSequence, s.textureFrame, s.gateParticles, s.position, s.yaw,
                        s.transform, s.placement, s.index, s.loop, s.holdPose);
    };
    // Empty figures have no artwork binding. Otherwise comparison requires the
    // actual shared prototype; equal names do not authenticate an asset owner.
    return (m_sim.tree == nullptr || m_sim.resources == other.m_sim.resources) &&
           values(m_sim) == values(other.m_sim) && m_sim.pose.equalLocal(other.m_sim.pose) &&
           m_sim.player.equalLocal(other.m_sim.player) &&
           m_sim.particles.equalLocal(other.m_sim.particles);
}

bool ItemFigure::registerNativeResources(NativeStateResources& registry) const {
    std::vector<const ItemArchive*> refreshed;
    const auto refresh = [&](const ItemArchive* archive) {
        if ((archive == nullptr) || std::ranges::find(refreshed, archive) != refreshed.end()) {
            return true;
        }
        if (!registry.refreshArchive(*archive)) {
            return false;
        }
        refreshed.push_back(archive);
        return true;
    };
    if (!refresh(m_sim.archive)) {
        return false;
    }
    const auto registerStatic = [&](const TreeInfo& tree, const ItemArchive& archive, u32 flags) {
        if (tree.nodes.size() != 1) {
            return false;
        }
        const auto object = archive.models.find(tree.nodes.front().object);
        return object && registry.registerStaticTree(tree, archive, *object, flags);
    };
    // Diagnostic prototype copies may own a static tree without a lazy binding
    // cache. Native copies normally find this same tree in the retained cache.
    if (m_sim.resources && m_sim.resources->staticTree &&
        (!m_sim.archive || m_sim.resources->staticTree->nodes.size() != 1 ||
         !registerStatic(*m_sim.resources->staticTree, *m_sim.archive,
                         m_sim.resources->staticTree->nodes.front().objectFlags))) {
        return false;
    }
    for (const auto& binding : *m_bindings) {
        if (!binding.archive || !binding.tree || !refresh(binding.archive)) {
            return false;
        }
        if (binding.staticModel) {
            // AssetScope may resolve a suffix (NAME -> NAMEL1); the generated
            // node records that actual object, unlike the requested cache name.
            if (!registerStatic(*binding.tree, *binding.archive, binding.flags)) {
                return false;
            }
        }
    }
    return registry.good();
}

TextureAnimator& ItemFigure::textures() const {
    auto& result = m_sim.resources->textures;
    result.restoreClock(m_sim.textureClock);
    return result;
}

Vec3 Obstacle::pushOut(const Vec3& position, f32 radius) const {
    if (!solid || !levelWith(*this, position)) {
        return position;
    }
    if (cylinderRadius > 0.0f) {
        Vec2 away{position.x - centre.x, position.z - centre.z};
        const f32 distance = glm::length(away);
        const f32 reach = cylinderRadius + radius;
        if (distance >= reach) {
            return position;
        }
        away = distance > 1e-5f ? away / distance : Vec2{1.0f, 0.0f};
        return Vec3{centre.x + away.x * reach, position.y, centre.z + away.y * reach};
    }
    const Vec2 local = localOf(*this, position);
    const Vec2 nearest{std::clamp(local.x, -halfAcross, halfAcross),
                       std::clamp(local.y, -halfAlong, halfAlong)};
    Vec2 away = local - nearest;
    f32 distance = glm::length(away);
    if (distance >= radius) {
        return position;
    }
    if (distance <= 1e-5f) {
        // Inside the box: out by the nearest side.
        const f32 toAcross = halfAcross - std::abs(local.x);
        const f32 toAlong = halfAlong - std::abs(local.y);
        away = toAcross < toAlong ? Vec2{local.x < 0.0f ? -1.0f : 1.0f, 0.0f}
                                  : Vec2{0.0f, local.y < 0.0f ? -1.0f : 1.0f};
        distance = -std::min(toAcross, toAlong);
    } else {
        away /= distance;
    }
    const Vec2 moved = local + away * (radius - distance);
    const f32 c = portable::cos(yaw);
    const f32 s = portable::sin(yaw);
    return Vec3{centre.x + moved.x * c + moved.y * s, position.y,
                centre.z - moved.x * s + moved.y * c};
}

bool Obstacle::touchedBy(const Vec3& position, f32 radius, f32 margin) const {
    if (!levelWith(*this, position)) {
        return false;
    }
    if (cylinderRadius > 0.0f) {
        return glm::length(Vec2{position.x - centre.x, position.z - centre.z}) <=
               cylinderRadius + radius + margin;
    }
    const Vec2 local = localOf(*this, position);
    const Vec2 nearest{std::clamp(local.x, -halfAcross, halfAcross),
                       std::clamp(local.y, -halfAlong, halfAlong)};
    return glm::length(local - nearest) <= radius + margin;
}

bool Obstacle::blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const {
    return contact(from, to, radius).has_value();
}

std::optional<f32> Obstacle::contact(const Vec3& from, const Vec3& to, f32 radius) const {
    if (!solid) {
        return std::nullopt;
    }
    radius = std::max(radius, 0.0f);
    f32 enter = 0.0f;
    f32 leave = 1.0f;
    const auto slab = [&](f32 start, f32 step, f32 low, f32 high) {
        if (step == 0.0f) {
            return start >= low && start <= high;
        }
        const f32 a = (low - start) / step;
        const f32 b = (high - start) / step;
        enter = std::max(enter, std::min(a, b));
        leave = std::min(leave, std::max(a, b));
        return enter <= leave;
    };
    if (!slab(from.y, to.y - from.y, centre.y - radius, centre.y + height + radius)) {
        return std::nullopt;
    }
    const Vec2 start = localOf(*this, from);
    const Vec2 step = localOf(*this, to) - start;
    if (cylinderRadius <= 0.0f) {
        if (slab(start.x, step.x, -halfAcross - radius, halfAcross + radius) &&
            slab(start.y, step.y, -halfAlong - radius, halfAlong + radius)) {
            return enter;
        }
        return std::nullopt;
    }
    const f32 reach = cylinderRadius + radius;
    const f32 a = glm::dot(step, step);
    const f32 b = glm::dot(start, step);
    const f32 c = glm::dot(start, start) - reach * reach;
    if (a == 0.0f) {
        return c <= 0.0f ? std::optional<f32>{enter} : std::nullopt;
    }
    const f32 discriminant = b * b - a * c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const f32 root = std::sqrt(discriminant);
    enter = std::max(enter, (-b - root) / a);
    leave = std::min(leave, (-b + root) / a);
    return enter <= leave ? std::optional<f32>{enter} : std::nullopt;
}

bool ItemFigure::place(RenderDevice& device, ItemArchive& items, std::string_view name,
                       const ItemInstance& instance, const WorldCollision* collision,
                       std::span<TextureSet* const> lenders) {
    m_sim.position = instance.position;
    if (collision != nullptr) {
        if (const auto floor = collision->restingFloorAt(instance.position, kFloorReachAbove,
                                                         kFloorReachBelow, kFloorReachRadius);
            floor.has_value()) {
            m_sim.position.y = floor->y + kFloorLift;
        }
    }
    m_sim.transform = itemPlacement(m_sim.position, instance.rotation);
    m_sim.yaw = portable::atan2(m_sim.transform[2].x, m_sim.transform[2].z);
    m_sim.placement = m_sim.transform;
    m_sim.tree = nullptr;
    m_sim.archive = &items;
    m_sim.poseSequence = 0;
    m_sim.poseFrame = 0;
    m_sim.poseGeneration = 0;
    ++m_sim.continuity;
    m_sim.textureClock = {};
    m_sim.index = -1;
    m_sim.player.stop();
    m_sim.particles = {};

    m_sim.textureSequence = 0;
    m_sim.textureFrame = 0;
    m_sim.gateParticles = false;
    m_sim.presentationCaptured = false;
    m_sim.presentationAdvanced = false;
    // A draw pose belongs to one tree topology and is never a rewind input.
    m_presentationPose = {};
    const auto tree = items.loaded() ? items.trees.find(name) : std::nullopt;
    if (!tree.has_value()) {
        m_sim.resources = std::make_shared<Resources>();
        return false;
    }
    m_sim.tree = &items.trees.tree(*tree);
    const auto cached = std::ranges::find_if(*m_bindings, [&](const CachedBinding& binding) {
        return binding.archive == &items && binding.device == &device &&
               binding.tree == m_sim.tree && !binding.staticModel &&
               std::ranges::equal(binding.lenders, lenders);
    });
    if (cached != m_bindings->end()) {
        m_sim.resources = cached->resources;
        m_sim.pose.rest(*m_sim.tree);
        m_sim.particles = cached->particles;
        m_sim.particles.step(0, m_sim.transform, m_sim.pose.matrices());
        play(0, Playback::Loop);
        return true;
    }
    m_sim.resources = std::make_shared<Resources>();
    const bool mesh =
        m_sim.resources->model.bind(*m_sim.tree, items.models, items.textures, device, lenders);
    const bool wantsMesh = std::ranges::any_of(m_sim.tree->nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
    m_sim.pose.rest(*m_sim.tree);
    m_sim.particles.bind(*m_sim.tree, items, device, m_sim.transform, m_sim.pose.matrices(),
                         lenders);
    if (!mesh && (wantsMesh || m_sim.particles.field().size() == 0)) {
        m_sim.tree = nullptr;
        return false;
    }
    textures().bind(items.trees.textureAnimations(), items.textures, device, lenders);
    m_bindings->push_back({&items,
                           &device,
                           std::string{name},
                           false,
                           0,
                           m_sim.tree,
                           m_sim.resources,
                           m_sim.particles,
                           {lenders.begin(), lenders.end()}});
    play(0, Playback::Loop);
    return true;
}

bool ItemFigure::place(RenderDevice& device, const AssetScope& scope, std::string_view name,
                       const ItemInstance& instance, const WorldCollision* collision,
                       std::span<TextureSet* const> lenders) {
    if (ItemArchive* archive = scope.archiveForTree(name)) {
        return place(device, *archive, name, instance, collision, lenders);
    }
    // An empty scope: the item still takes its place, with nothing to show.
    static ItemArchive none;
    const bool placed = place(device, none, name, instance, collision, lenders);
    // This placeholder is not an authored archive in the level's resource
    // catalog. Empty figures retain their placement, not a synthetic binding.
    m_sim.archive = nullptr;
    return placed;
}

bool ItemFigure::placeStaticFallback(RenderDevice& device, const AssetScope& scope,
                                     std::string_view name, const ItemInstance& instance,
                                     const WorldCollision* collision, u32 objectFlags,
                                     std::span<TextureSet* const> lenders) {
    if (place(device, scope, name, instance, collision, lenders)) {
        return true;
    }
    // A present but invalid tree is not a request to silently substitute another mesh.
    if (scope.findTree(name)) {
        return false;
    }
    const auto model = scope.findObject(name);
    if (!model) {
        return false;
    }
    ItemArchive& items = *model->archive;
    m_sim.archive = &items;
    const auto cached = std::ranges::find_if(*m_bindings, [&](const CachedBinding& binding) {
        return binding.archive == &items && binding.device == &device && binding.name == name &&
               binding.staticModel && binding.flags == objectFlags &&
               std::ranges::equal(binding.lenders, lenders);
    });
    if (cached != m_bindings->end()) {
        m_sim.resources = cached->resources;
        m_sim.tree = cached->tree;
        m_sim.pose.rest(*m_sim.tree);
        m_sim.holdPose = false;
        m_sim.index = 0;
        return true;
    }
    m_sim.resources->staticTree = std::make_shared<TreeInfo>();
    m_sim.resources->staticTree->name = name;
    TreeNodeInfo node;
    node.name = name;
    node.object = model->name;
    node.objectFlags = objectFlags;
    m_sim.resources->staticTree->nodes.push_back(std::move(node));
    if (!m_sim.resources->model.bind(*m_sim.resources->staticTree, items.models, items.textures,
                                     device, lenders)) {
        m_sim.resources->staticTree.reset();
        return false;
    }
    m_sim.tree = m_sim.resources->staticTree.get();
    m_sim.pose.rest(*m_sim.tree);
    textures().bind(items.trees.textureAnimations(), items.textures, device, lenders);
    m_sim.holdPose = false;
    m_sim.index = 0;
    m_bindings->push_back({&items,
                           &device,
                           std::string{name},
                           true,
                           objectFlags,
                           m_sim.tree,
                           m_sim.resources,
                           {},
                           {lenders.begin(), lenders.end()}});
    return true;
}

void ItemFigure::gateParticlesOnSequence(bool enabled) {
    // AnimateNode's particle branch (80011334): sequence zero sets 0x200000
    // to suppress births, while MBDrawPsys continues aging existing particles.
    m_sim.gateParticles = enabled;
    m_sim.particles.setEmitting(!enabled || m_sim.index != 0);
}

void ItemFigure::tilt(f32 pitch, f32 yaw) {
    m_sim.transform = itemPlacement(m_sim.position, Vec3{pitch, m_sim.yaw + yaw, 0.0f});
}

void ItemFigure::placeAt(const Mat4& placement) {
    if (glm::distance(Vec3{m_sim.transform[3]}, Vec3{placement[3]}) > kPresentationCutDistance) {
        ++m_sim.continuity;
    }
    m_sim.transform = placement;
    m_sim.placement = placement;
    m_sim.position = Vec3{placement[3]};
    m_sim.yaw = portable::atan2(placement[2].x, placement[2].z);
}

void ItemFigure::play(s32 index, Playback playback) {
    const bool hadPose = m_sim.index >= 0;
    m_sim.index = index;
    m_sim.loop = playback == Playback::Loop;
    m_sim.particles.setEmitting(!m_sim.gateParticles || index != 0);
    if (m_sim.tree == nullptr) {
        return;
    }
    if (index < 0 || static_cast<usize>(index) >= m_sim.tree->sequences.size()) {
        // Particle-only trees still own free-running texture cycles.
        refreshTextures();
        return;
    }
    m_sim.player.start(m_sim.tree->sequences[static_cast<usize>(index)], static_cast<u32>(index));
    const auto& sequence = m_sim.tree->sequences[static_cast<usize>(index)];
    // Atree's empty state changes object visibility, not the previous pose.
    // CHEST OPEN must retain the final ACTIVE lid/socket transforms.
    m_sim.holdPose = hadPose && sequence.frames == 0 && sequence.tracks.empty();
    if (!m_sim.holdPose) {
        m_sim.pose.evaluate(*m_sim.tree, static_cast<u32>(index), 0.0f);
        m_sim.poseSequence = static_cast<u32>(index);
        m_sim.poseFrame = 0;
        m_sim.poseGeneration = m_sim.player.generation();
    }
    m_sim.resources->model.setFrame(static_cast<u32>(index), 0);
    m_sim.particles.setLocalScales(m_sim.pose.poses());
    refreshTextures();
}

void ItemFigure::capturePresentation() {
    m_sim.previousTransform = m_sim.transform;
    m_sim.previousFrame = m_sim.player.presentationFrame();
    textures().advance(0);
    m_sim.textureClock = m_sim.resources->textures.clock();
    m_sim.previousGeneration = m_sim.player.generation();
    m_sim.presentationCaptured = true;
    m_sim.presentationAdvanced = false;
}

void ItemFigure::update(f32 seconds) {
    if (m_sim.tree == nullptr) {
        return;
    }
    m_sim.player.advance(seconds, m_sim.loop);
    if (!m_sim.holdPose && m_sim.player.playing()) {
        m_sim.pose.evaluate(*m_sim.tree, m_sim.player.sequence(), m_sim.player.frame());
        m_sim.poseSequence = m_sim.player.sequence();
        m_sim.poseFrame = m_sim.player.frame();
        m_sim.poseGeneration = m_sim.player.generation();
    }
    m_sim.resources->model.setFrame(m_sim.player.sequence(),
                                    static_cast<s32>(m_sim.player.frame()));
    textures().advance(seconds);
    m_sim.textureClock = m_sim.resources->textures.clock();
    refreshTextures();
    m_sim.particles.setLocalScales(m_sim.pose.poses());
    m_sim.particles.step(seconds, m_sim.transform, m_sim.pose.matrices());
    m_sim.presentationAdvanced = seconds > 0;
}

void ItemFigure::refreshTextures() {
    // DoSeqTexMods changes alternate textures only when the sequence has texmods.
    // An empty OFF state retains ONB's final transparent frame, rather than restoring
    // the archive's luminous FFGEN14 default. Free-running textures still advance.
    if (m_sim.player.sequence() < m_sim.tree->sequences.size()) {
        const auto& sequence = m_sim.tree->sequences[m_sim.player.sequence()];
        if (!m_sim.holdPose || sequence.textureAnimationCount > 0) {
            m_sim.textureSequence = m_sim.player.sequence();
            m_sim.textureFrame = static_cast<s32>(m_sim.player.frame());
        }
    }
    // Missing sequences do not suppress clock-driven cycles. Updating the base
    // binding here keeps non-interpolated cutouts independent of draw cadence.
    textures().apply(m_sim.resources->model, *m_sim.tree, m_sim.textureSequence,
                     m_sim.textureFrame);
    textures().apply(m_sim.particles, *m_sim.tree, m_sim.textureSequence, m_sim.textureFrame);
}

ItemFigure::Presentation ItemFigure::presentation() const {
    return {m_sim.tree,
            m_sim.archive,
            m_sim.poseSequence,
            m_sim.poseFrame,
            m_sim.poseGeneration,
            m_sim.player.sequence(),
            m_sim.player.frame(),
            m_sim.textureSequence,
            static_cast<f32>(m_sim.textureFrame),
            static_cast<f32>(textures().frame()),
            m_sim.continuity,
            m_sim.tree != nullptr && m_sim.resources->model.bound()};
}

bool ItemFigure::finished() const {
    return m_sim.tree == nullptr || !m_sim.player.playing() || m_sim.player.finished();
}

std::optional<Mat4> ItemFigure::nodeTransform(std::string_view name) const {
    if (m_sim.tree != nullptr) {
        for (usize i = 0; i < m_sim.tree->nodes.size(); ++i) {
            if (m_sim.tree->nodes[i].name == name && i < m_sim.pose.matrices().size()) {
                return m_sim.transform * m_sim.pose.matrices()[i];
            }
        }
    }
    return std::nullopt;
}

f32 ItemFigure::progress() const {
    return m_sim.player.frameCount() > 1
               ? std::clamp((m_sim.player.frame() + 1) /
                                static_cast<f32>(m_sim.player.frameCount()),
                            0.0f, 1.0f)
               : 1.0f;
}

const TreeSequenceInfo* ItemFigure::sequenceInfo(s32 index) const {
    if (m_sim.tree == nullptr || index < 0 ||
        static_cast<usize>(index) >= m_sim.tree->sequences.size()) {
        return nullptr;
    }
    return &m_sim.tree->sequences[static_cast<usize>(index)];
}

s32 ItemFigure::ticksOf(s32 index) const {
    if (m_sim.tree == nullptr || index < 0 ||
        static_cast<usize>(index) >= m_sim.tree->sequences.size()) {
        return 0;
    }
    const TreeSequenceInfo& info = m_sim.tree->sequences[static_cast<usize>(index)];
    const f32 rate =
        info.frameRate > 0 ? static_cast<f32>(info.frameRate) : AnimationPlayer::kDefaultRate;
    return static_cast<s32>(std::ceil(static_cast<f32>(info.frames) * rate *
                                      AnimationPlayer::kRateUnit * kTicksPerSecond));
}

void ItemFigure::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                      f32 alpha, f32 scale, const CameraFrame* camera, TreeModel::Pass pass,
                      f32 presentationAlpha, const Appearance* appearance) const {
    if (m_sim.tree != nullptr) {
        Mat4 placement = m_sim.transform;
        const TreePose* pose = &m_sim.pose;
        f32 visualFrame = static_cast<f32>(m_sim.textureFrame);
        if (presentationAlpha >= 0 && m_sim.presentationCaptured) {
            const f32 blend = std::clamp(presentationAlpha, 0.0f, 1.0f);
            if (glm::distance(Vec3{m_sim.previousTransform[3]}, Vec3{m_sim.transform[3]}) <=
                kPresentationCutDistance) {
                placement = blendPlacement(m_sim.previousTransform, m_sim.transform, blend);
            }
            if (!m_sim.holdPose && m_sim.player.playing()) {
                const f32 frame =
                    m_sim.previousGeneration == m_sim.player.generation()
                        ? glm::mix(m_sim.previousFrame, m_sim.player.presentationFrame(), blend)
                        : m_sim.player.presentationFrame();
                m_presentationPose.evaluate(*m_sim.tree, m_sim.player.sequence(), frame,
                                            TreePose::Sampling::Smooth);
                pose = &m_presentationPose;
                visualFrame = frame;
            }
        }
        textures().apply(m_sim.resources->model, *m_sim.tree, m_sim.textureSequence, visualFrame,
                         textures().presentationOffset(presentationAlpha));
        auto& model = m_sim.resources->model;
        // ItemFigure copies share their immutable render bindings. Reapply defaults
        // as well as overrides so a locked/tinted copy cannot affect its siblings.
        model.setAppearance({.unlit = appearance != nullptr && appearance->unlit,
                             .tint = appearance != nullptr ? appearance->tint : Color::white()});
        model.setWhiteBlend(appearance != nullptr ? appearance->whiteBlend : 0.0f);
        model.setDoubleSided(appearance != nullptr && appearance->doubleSided);
        if (appearance != nullptr) {
            for (const usize node : appearance->hiddenMeshes) {
                model.setMeshAlpha(node, 0);
            }
        }
        if (!m_sim.holdPose) {
            m_sim.resources->model.setPresentationFrame(m_sim.player.sequence(), visualFrame);
        } else {
            m_sim.resources->model.setFrame(m_sim.player.sequence(),
                                            static_cast<s32>(m_sim.player.frame()));
        }
        m_sim.resources->model.draw(device, clip, glm::scale(placement, Vec3{scale}), lighting,
                                    pose->matrices(), camera, alpha, pass);
        // Particles composite with the blended parts: once, after the solids, so a figure
        // drawn in both pickup passes does not double them.
        if (TreeModel::compositesEffects(pass)) {
            const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
            textures().applyPresentation(m_sim.particles, *m_sim.tree, m_sim.textureSequence,
                                         visualFrame,
                                         textures().presentationOffset(presentationAlpha));
            m_sim.particles.draw(device, clip, frame.right, frame.up,
                                 m_sim.presentationAdvanced ? presentationAlpha : -1.0f,
                                 kThermalTextures);
        }
    }
}

Mat4 itemPlacement(const Vec3& position, const Vec3& rotation) {
    // Item angles are world-axis pitch, yaw, roll. GLM post-multiplies, so compose
    // them in reverse order; the authored yaw has the opposite sign to GLM's Y turn.
    Mat4 transform = glm::translate(Mat4{1.0f}, position);
    transform = portable::rotate(transform, rotation.z, Vec3{0.0f, 0.0f, 1.0f});
    transform = portable::rotate(transform, -rotation.y, Vec3{0.0f, 1.0f, 0.0f});
    return portable::rotate(transform, rotation.x, Vec3{1.0f, 0.0f, 0.0f});
}

Obstacle ItemFigure::obstacle(const ItemInfo& info) const {
    Obstacle box;
    box.centre = m_sim.position;
    box.yaw = m_sim.yaw;
    // A record without a box is as wide as its radius.
    box.halfAcross = info.xSize > 0.0f ? info.xSize : info.radius;
    box.halfAlong = info.zSize > 0.0f ? info.zSize : info.radius;
    box.height = info.height;
    if (info.collisionType == 1 || info.collisionType == 3) {
        box.enemyItem = Obstacle::ItemQuery{info.collisionType == 1, info.radius};
    }
    return box;
}

bool shownToParty(s32 minPlayers, s32 players) {
    if (minPlayers > kExactPlayersMark) {
        return players == minPlayers - kExactPlayersMark;
    }
    return players >= minPlayers;
}

} // namespace gdl::game
