#include "game/world/ItemFigure.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr f32 kFloorReachAbove = 0.5f;
constexpr f32 kFloorReachBelow = 3.0f;
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
        result = glm::mat4_cast(glm::slerp(glm::quat_cast(from), glm::quat_cast(to), blend));
        result = glm::scale(result, glm::mix(fromScale, toScale, blend));
    }
    result[3] = glm::mix(previous[3], current[3], blend);
    return result;
}

/** `position` in the box's own space, about its centre. */
Vec2 localOf(const Obstacle& box, const Vec3& position) {
    const f32 dx = position.x - box.centre.x;
    const f32 dz = position.z - box.centre.z;
    const f32 c = std::cos(box.yaw);
    const f32 s = std::sin(box.yaw);
    return Vec2{dx * c - dz * s, dx * s + dz * c};
}

bool levelWith(const Obstacle& box, const Vec3& position) {
    return position.y <= box.centre.y + box.height && position.y + box.height >= box.centre.y;
}

} // namespace

ItemArchive& itemArchiveForTree(ItemArchive& items, std::string_view name,
                                ItemArchive* realmItems) {
    return !items.trees.find(name) && realmItems != nullptr ? *realmItems : items;
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
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
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
                       const ItemInstance& instance, const WorldCollision* collision) {
    m_position = instance.position;
    if (collision != nullptr) {
        if (const auto floor =
                collision->floorAt(instance.position, kFloorReachAbove, kFloorReachBelow);
            floor.has_value()) {
            m_position.y = floor->y + kFloorLift;
        }
    }
    m_transform = itemPlacement(m_position, instance.rotation);
    m_yaw = std::atan2(m_transform[2].x, m_transform[2].z);
    m_placement = m_transform;
    m_tree = nullptr;
    m_archive = &items;
    m_poseSequence = 0;
    m_poseFrame = 0;
    m_poseGeneration = 0;
    ++m_continuity;
    m_staticTree.reset();
    m_index = -1;
    m_player.stop();
    m_particles = {};
    m_textures.clear();
    m_textureSequence = 0;
    m_textureFrame = 0;
    m_gateParticles = false;
    m_presentationCaptured = false;
    m_presentationAdvanced = false;
    const auto tree = items.loaded() ? items.trees.find(name) : std::nullopt;
    if (!tree.has_value()) {
        return false;
    }
    m_tree = &items.trees.tree(*tree);
    const bool mesh = m_model.bind(*m_tree, items.models, items.textures, device);
    const bool wantsMesh = std::ranges::any_of(m_tree->nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
    m_pose.rest(*m_tree);
    m_particles.bind(*m_tree, items, device, m_transform, m_pose.matrices());
    if (!mesh && (wantsMesh || m_particles.field().size() == 0)) {
        m_tree = nullptr;
        return false;
    }
    m_textures.bind(items.trees.textureAnimations(), items.textures, device);
    play(0, true);
    return true;
}

bool ItemFigure::placeStaticFallback(RenderDevice& device, ItemArchive& items,
                                     std::string_view name, const ItemInstance& instance,
                                     const WorldCollision* collision, u32 objectFlags) {
    if (place(device, items, name, instance, collision)) {
        return true;
    }
    // A present but invalid tree is not a request to silently substitute another mesh.
    if (!items.loaded() || items.trees.find(name)) {
        return false;
    }
    for (const char* suffix : {"", "L1", "L1ROOT"}) {
        const auto model = items.models.find(std::string{name} + suffix);
        if (!model) {
            continue;
        }
        m_staticTree = std::make_unique<TreeInfo>();
        m_staticTree->name = name;
        TreeNodeInfo node;
        node.name = name;
        node.object = items.models.entry(*model).name;
        node.objectFlags = objectFlags;
        m_staticTree->nodes.push_back(std::move(node));
        if (!m_model.bind(*m_staticTree, items.models, items.textures, device)) {
            m_staticTree.reset();
            return false;
        }
        m_tree = m_staticTree.get();
        m_pose.rest(*m_tree);
        m_textures.bind(items.trees.textureAnimations(), items.textures, device);
        m_holdPose = false;
        m_index = 0;
        return true;
    }
    return false;
}

void ItemFigure::gateParticlesOnSequence(bool enabled) {
    // AnimateNode's particle branch (80011334): sequence zero sets 0x200000
    // to suppress births, while MBDrawPsys continues aging existing particles.
    m_gateParticles = enabled;
    m_particles.setEmitting(!enabled || m_index != 0);
}

void ItemFigure::tilt(f32 pitch, f32 yaw) {
    m_transform = itemPlacement(m_position, Vec3{pitch, m_yaw + yaw, 0.0f});
}

void ItemFigure::placeAt(const Mat4& placement) {
    if (glm::distance(Vec3{m_transform[3]}, Vec3{placement[3]}) > kPresentationCutDistance) {
        ++m_continuity;
    }
    m_transform = placement;
    m_placement = placement;
    m_position = Vec3{placement[3]};
    m_yaw = std::atan2(placement[2].x, placement[2].z);
}

void ItemFigure::play(s32 index, bool loop) {
    const bool hadPose = m_index >= 0;
    m_index = index;
    m_loop = loop;
    m_particles.setEmitting(!m_gateParticles || index != 0);
    if (m_tree == nullptr || index < 0 || static_cast<usize>(index) >= m_tree->sequences.size()) {
        return;
    }
    m_player.start(m_tree->sequences[static_cast<usize>(index)], static_cast<u32>(index));
    const auto& sequence = m_tree->sequences[static_cast<usize>(index)];
    // Atree's empty state changes object visibility, not the previous pose.
    // CHEST OPEN must retain the final ACTIVE lid/socket transforms.
    m_holdPose = hadPose && sequence.frames == 0 && sequence.tracks.empty();
    if (!m_holdPose) {
        m_pose.evaluate(*m_tree, static_cast<u32>(index), 0.0f);
        m_poseSequence = static_cast<u32>(index);
        m_poseFrame = 0;
        m_poseGeneration = m_player.generation();
    }
    m_model.setFrame(static_cast<u32>(index), 0);
    m_particles.setLocalScales(m_pose.poses());
    refreshTextures();
}

void ItemFigure::capturePresentation() {
    m_previousTransform = m_transform;
    m_previousFrame = m_player.presentationFrame();
    m_textures.advance(0);
    m_previousGeneration = m_player.generation();
    m_presentationCaptured = true;
    m_presentationAdvanced = false;
}

void ItemFigure::update(f32 seconds) {
    if (m_tree == nullptr) {
        return;
    }
    m_player.advance(seconds, m_loop);
    if (!m_holdPose && m_player.playing()) {
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
        m_poseSequence = m_player.sequence();
        m_poseFrame = m_player.frame();
        m_poseGeneration = m_player.generation();
    }
    m_model.setFrame(m_player.sequence(), static_cast<s32>(m_player.frame()));
    m_textures.advance(seconds);
    refreshTextures();
    m_particles.setLocalScales(m_pose.poses());
    m_particles.step(seconds, m_transform, m_pose.matrices());
    m_presentationAdvanced = seconds > 0;
}

void ItemFigure::refreshTextures() {
    if (m_player.sequence() >= m_tree->sequences.size()) {
        return;
    }
    // DoSeqTexMods changes alternate textures only when the sequence has texmods.
    // An empty OFF state retains ONB's final transparent frame, rather than restoring
    // the archive's luminous FFGEN14 default. Free-running textures still advance.
    const auto& sequence = m_tree->sequences[m_player.sequence()];
    if (!m_holdPose || sequence.textureAnimationCount > 0) {
        m_textureSequence = m_player.sequence();
        m_textureFrame = static_cast<s32>(m_player.frame());
    }
    m_textures.apply(m_model, *m_tree, m_textureSequence, m_textureFrame);
    m_textures.apply(m_particles, *m_tree, m_textureSequence, m_textureFrame);
}

ItemFigure::Presentation ItemFigure::presentation() const {
    return {m_tree,
            m_archive,
            m_poseSequence,
            m_poseFrame,
            m_poseGeneration,
            m_player.sequence(),
            m_player.frame(),
            m_textureSequence,
            static_cast<f32>(m_textureFrame),
            static_cast<f32>(m_textures.frame()),
            m_continuity,
            m_tree != nullptr && m_model.bound()};
}

bool ItemFigure::finished() const {
    return m_tree == nullptr || !m_player.playing() || m_player.finished();
}

std::optional<Mat4> ItemFigure::nodeTransform(std::string_view name) const {
    if (m_tree != nullptr) {
        for (usize i = 0; i < m_tree->nodes.size(); ++i) {
            if (m_tree->nodes[i].name == name && i < m_pose.matrices().size()) {
                return m_transform * m_pose.matrices()[i];
            }
        }
    }
    return std::nullopt;
}

f32 ItemFigure::progress() const {
    return m_player.frameCount() > 1
               ? std::clamp((m_player.frame() + 1) / static_cast<f32>(m_player.frameCount()), 0.0f,
                            1.0f)
               : 1.0f;
}

const TreeSequenceInfo* ItemFigure::sequenceInfo(s32 index) const {
    if (m_tree == nullptr || index < 0 || static_cast<usize>(index) >= m_tree->sequences.size()) {
        return nullptr;
    }
    return &m_tree->sequences[static_cast<usize>(index)];
}

s32 ItemFigure::ticksOf(s32 index) const {
    if (m_tree == nullptr || index < 0 || static_cast<usize>(index) >= m_tree->sequences.size()) {
        return 0;
    }
    const TreeSequenceInfo& info = m_tree->sequences[static_cast<usize>(index)];
    const f32 rate =
        info.frameRate > 0 ? static_cast<f32>(info.frameRate) : AnimationPlayer::kDefaultRate;
    return static_cast<s32>(std::ceil(static_cast<f32>(info.frames) * rate *
                                      AnimationPlayer::kRateUnit * kTicksPerSecond));
}

void ItemFigure::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                      f32 alpha, f32 scale, const CameraFrame* camera, TreeModel::Pass pass,
                      f32 presentationAlpha) const {
    if (m_tree != nullptr) {
        Mat4 placement = m_transform;
        const TreePose* pose = &m_pose;
        f32 visualFrame = static_cast<f32>(m_textureFrame);
        if (presentationAlpha >= 0 && m_presentationCaptured) {
            const f32 blend = std::clamp(presentationAlpha, 0.0f, 1.0f);
            if (glm::distance(Vec3{m_previousTransform[3]}, Vec3{m_transform[3]}) <=
                kPresentationCutDistance) {
                placement = blendPlacement(m_previousTransform, m_transform, blend);
            }
            if (!m_holdPose && m_player.playing()) {
                const f32 frame =
                    m_previousGeneration == m_player.generation()
                        ? glm::mix(m_previousFrame, m_player.presentationFrame(), blend)
                        : m_player.presentationFrame();
                m_presentationPose.evaluate(*m_tree, m_player.sequence(), frame, false, true);
                pose = &m_presentationPose;
                visualFrame = frame;
            }
        }
        m_textures.apply(m_model, *m_tree, m_textureSequence, visualFrame,
                         m_textures.presentationOffset(presentationAlpha));
        if (!m_holdPose) {
            m_model.setPresentationFrame(m_player.sequence(), visualFrame);
        }
        m_model.draw(device, clip, glm::scale(placement, Vec3{scale}), lighting, pose->matrices(),
                     camera, alpha, pass);
        if (pass != TreeModel::Pass::DepthWriting) {
            const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
            m_textures.apply(m_particles, *m_tree, m_textureSequence, visualFrame,
                             m_textures.presentationOffset(presentationAlpha));
            m_particles.draw(device, clip, frame.right, frame.up,
                             m_presentationAdvanced ? presentationAlpha : -1.0f);
        }
    }
}

Mat4 itemPlacement(const Vec3& position, const Vec3& rotation) {
    // Item angles are world-axis pitch, yaw, roll. GLM post-multiplies, so compose
    // them in reverse order; the authored yaw has the opposite sign to GLM's Y turn.
    Mat4 transform = glm::translate(Mat4{1.0f}, position);
    transform = glm::rotate(transform, rotation.z, Vec3{0.0f, 0.0f, 1.0f});
    transform = glm::rotate(transform, -rotation.y, Vec3{0.0f, 1.0f, 0.0f});
    return glm::rotate(transform, rotation.x, Vec3{1.0f, 0.0f, 0.0f});
}

Obstacle ItemFigure::obstacle(const ItemInfo& info) const {
    Obstacle box;
    box.centre = m_position;
    box.yaw = m_yaw;
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
