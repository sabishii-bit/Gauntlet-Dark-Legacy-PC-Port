#include "game/world/ItemFigure.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

namespace {

constexpr f32 kFloorReachAbove = 0.5f;
constexpr f32 kFloorReachBelow = 3.0f;
constexpr f32 kTicksPerSecond = 60.0f;
constexpr s32 kExactPlayersMark = 10;

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

Vec3 Obstacle::pushOut(const Vec3& position, f32 radius) const {
    if (!solid || !levelWith(*this, position)) {
        return position;
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
    const Vec2 local = localOf(*this, position);
    const Vec2 nearest{std::clamp(local.x, -halfAcross, halfAcross),
                       std::clamp(local.y, -halfAlong, halfAlong)};
    return glm::length(local - nearest) <= radius + margin;
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
    m_yaw = instance.rotation.y;
    m_transform = itemPlacement(m_position, instance.rotation);
    m_tree = nullptr;
    m_index = -1;
    const auto tree = items.loaded() ? items.trees.find(name) : std::nullopt;
    if (!tree.has_value() ||
        !m_model.bind(items.trees.tree(*tree), items.models, items.textures, device)) {
        return false;
    }
    m_tree = &items.trees.tree(*tree);
    m_pose.rest(*m_tree);
    play(0, true);
    return true;
}

void ItemFigure::play(s32 index, bool loop) {
    m_index = index;
    m_loop = loop;
    if (m_tree == nullptr || index < 0 || static_cast<usize>(index) >= m_tree->sequences.size()) {
        return;
    }
    m_player.start(m_tree->sequences[static_cast<usize>(index)], static_cast<u32>(index));
    m_pose.evaluate(*m_tree, static_cast<u32>(index), 0.0f);
    m_model.setFrame(static_cast<u32>(index), 0);
}

void ItemFigure::update(f32 seconds) {
    if (m_tree == nullptr || !m_player.playing()) {
        return;
    }
    m_player.advance(seconds, m_loop);
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    m_model.setFrame(m_player.sequence(), static_cast<s32>(m_player.frame()));
}

bool ItemFigure::finished() const {
    return m_tree == nullptr || !m_player.playing() || m_player.finished();
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

void ItemFigure::draw(RenderDevice& device, const Mat4& clip,
                      const WorldLighting& lighting) const {
    if (m_tree != nullptr) {
        m_model.draw(device, clip, m_transform, lighting, m_pose.matrices());
    }
}

Mat4 itemPlacement(const Vec3& position, const Vec3& rotation) {
    Mat4 transform = glm::translate(Mat4{1.0f}, position);
    transform = glm::rotate(transform, rotation.y, Vec3{0.0f, 1.0f, 0.0f});
    transform = glm::rotate(transform, -rotation.x, Vec3{1.0f, 0.0f, 0.0f});
    return glm::rotate(transform, rotation.z, Vec3{0.0f, 0.0f, 1.0f});
}

Obstacle ItemFigure::obstacle(const ItemInfo& info) const {
    Obstacle box;
    box.centre = m_position;
    box.yaw = m_yaw;
    // A record without a box is as wide as its radius.
    box.halfAcross = info.xSize > 0.0f ? info.xSize : info.radius;
    box.halfAlong = info.zSize > 0.0f ? info.zSize : info.radius;
    box.height = info.height;
    return box;
}

bool shownToParty(s32 minPlayers, s32 players) {
    if (minPlayers > kExactPlayersMark) {
        return players == minPlayers - kExactPlayersMark;
    }
    return players >= minPlayers;
}

} // namespace gdl::game
