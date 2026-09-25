#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
namespace gdl::game {
namespace {
constexpr f32 kPi = std::numbers::pi_v<f32>;
f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

namespace {
constexpr s32 kThawBlinkTicks = 180;
constexpr s32 kThawBlinkBit = 8;
} // namespace
Mat4 Combatant::modelTransform(const Actor& critter) {
    // The original places the root at floor Y + floorOffset, then transforms originOffset
    // and the animated nodes from that root. Keep our floor probes at the ground anchor:
    // the Dragon's root is 18.5 units above it, outside the probe's vertical search range.
    const Vec3 root = critter.position + Vec3{0.0f, critter.definition->floorOffset(), 0.0f};
    const Mat4 model =
        glm::rotate(glm::translate(Mat4{1.0f}, root), critter.yaw, Vec3{0.0f, 1.0f, 0.0f});
    return glm::scale(model, Vec3{critter.scale});
}

Vec3 Combatant::partPosition(const Actor& critter, std::string_view node) {
    return Vec3{partTransform(critter, node)[3]};
}

Mat4 Combatant::partTransform(const Actor& critter, std::string_view node) {
    if (node.empty() || !critter.stock->tree->findNode(node).has_value()) {
        return glm::translate(modelTransform(critter), critter.definition->originOffset());
    }
    return attachmentTransform(critter, node);
}

Mat4 Combatant::attachmentTransform(const Actor& critter, std::string_view node) {
    const Mat4 model = modelTransform(critter);
    if (!node.empty()) {
        if (const auto index = critter.stock->tree->findNode(node); index.has_value()) {
            const std::span<const Mat4> matrices = critter.pose.matrices();
            if (*index < matrices.size()) {
                return model * matrices[*index];
            }
        }
    }
    return model;
}

std::optional<Mat4> Combatant::nodeTransform(std::string_view node) const {
    if (present()) {
        if (const auto index = m_actor.stock->tree->findNode(node)) {
            for (const auto& part : m_children) {
                s32 ancestor = static_cast<s32>(*index);
                while (ancestor >= 0) {
                    if (part->m_actor.branch == static_cast<usize>(ancestor)) {
                        return attachmentTransform(part->m_actor, node);
                    }
                    ancestor = m_actor.stock->tree->nodes[static_cast<usize>(ancestor)].parent;
                }
            }
        }
    }
    return present() ? std::optional{attachmentTransform(m_actor, node)} : std::nullopt;
}
std::optional<Mat4> Combatant::rootTransform() const {
    return present() ? std::optional{attachmentTransform(m_actor, data()->rootNode())}
                     : std::nullopt;
}
std::vector<MissileTarget> Combatant::bodyTargets(bool solidOnly) const {
    auto out = ownTargets(solidOnly);
    if (alive()) {
        for (const auto& part : m_children) {
            const auto volumes = part->ownTargets(solidOnly);
            out.insert(out.end(), volumes.begin(), volumes.end());
        }
    }
    return out;
}
std::vector<MissileTarget> Combatant::ownTargets(bool solidOnly) const {
    std::vector<MissileTarget> out;
    if (!alive()) {
        return out;
    }
    const auto& actor = m_actor;
    for (const auto& part : data()->parts()) {
        if (part.radius <= 0 || (solidOnly && (part.flags & CritterPart::kSolid) == 0) ||
            !actor.stock->tree->findNode(part.node).has_value()) {
            continue;
        }
        const Vec3 centre{attachmentTransform(actor, part.node) * Vec4{part.position, 1}};
        const f32 radius = part.radius * actor.scale;
        out.push_back({id(), centre - Vec3{0, radius, 0}, radius, 2 * radius});
    }
    if (data()->parts().empty()) {
        out.push_back({id(), position(), radius() * actor.scale, 8 * actor.scale});
    } else if (!actor.branch.has_value()) {
        const Vec3 centre = partPosition(actor, {});
        const f32 radius = (solidOnly ? data()->wallRadius() : data()->radius()) * actor.scale;
        out.push_back({id(), centre - Vec3{0, radius, 0}, radius, 2 * radius});
    }
    return out;
}
void Combatant::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     const Texture* frozenTexture) const {
    const Actor& critter = m_actor;
    if (critter.state == State::Inactive || critter.stock == nullptr) {
        return;
    }
    // The model is shared by this species, but object-frame selection belongs to the
    // individual. Set it for every draw, including the first and frozen frames.
    critter.stock->body.setFrame(critter.player.sequence(),
                                 static_cast<s32>(critter.player.frame()));
    critter.stock->textures.apply(critter.stock->body, *critter.stock->tree,
                                  critter.player.sequence(),
                                  static_cast<s32>(critter.player.frame()));
    critter.stock->body.setAppearance(false, critter.tint);
    // Retail flashes the normal skin on bit 3 in the final 180 frozen ticks.
    if (frozenTexture != nullptr && critter.frozenTicks > 0 &&
        (critter.frozenTicks >= kThawBlinkTicks || (critter.frozenTicks & kThawBlinkBit) == 0)) {
        critter.stock->body.setMaskedTexture(frozenTexture);
    }
    TreePose pose = critter.pose;
    for (const auto& part : m_children) {
        const Actor& branch = part->m_actor;
        if (branch.branch.has_value()) {
            pose.overlaySubtree(branch.pose, *branch.branch);
            critter.stock->body.setSubtreeFrame(*branch.branch, branch.player.sequence(),
                                                static_cast<s32>(branch.player.frame()));
            if (branch.hidden) {
                critter.stock->body.setNodeAlpha(*branch.branch, 0.0f);
            }
        }
    }
    critter.stock->body.draw(device, clip, modelTransform(critter), lighting, pose.matrices(),
                             nullptr, critter.alpha);
}

std::optional<f32> Combatant::contactDistance(const Vec3& from, const Vec3& to, f32 radius) const {
    if (!alive()) {
        return std::nullopt;
    }
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    std::optional<f32> best;
    for (const auto& body : bodyTargets()) {
        const Vec3 centre = body.base + Vec3{0, body.height * 0.5f, 0};
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0;
        const Vec3 nearest = from + sweep * t;
        const f32 distance = t * length;
        if (flatDistance(nearest, centre) <= radius + body.radius &&
            std::abs(nearest.y - centre.y) <= radius + body.height * 0.5f &&
            (!best.has_value() || distance < *best)) {
            best = distance;
        }
    }
    return best;
}
bool Combatant::within(const Vec3& centre, f32 radius) const {
    return alive() && glm::length(position() + Vec3{0, 4, 0} - centre) <= radius + this->radius();
}
bool Combatant::reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const {
    if (!alive()) {
        return false;
    }
    const f32 distance = flatDistance(position(), centre);
    if (distance > radius + this->radius()) {
        return false;
    }
    if (arc < kPi && distance > 0.001f) {
        const Vec3 toward = position() - centre;
        const f32 angle = std::acos(
            std::clamp((toward.x * facing.x + toward.z * facing.z) / distance, -1.0f, 1.0f));
        if (angle > arc) {
            return false;
        }
    }
    return true;
}
s32 Combatant::moveType() const {
    return m_actor.move >= 0 && data() != nullptr
               ? data()->moves()[static_cast<usize>(m_actor.move)].type
               : -1;
}
std::string_view Combatant::moveName() const {
    return m_actor.move >= 0 && data() != nullptr
               ? data()->moves()[static_cast<usize>(m_actor.move)].name
               : std::string_view{};
}
std::string Combatant::form() const {
    return m_actor.stock != nullptr ? m_actor.stock->definition.dropForm : std::string{};
}
const CritterData* Combatant::data() const {
    return m_actor.stock != nullptr ? m_actor.definition : nullptr;
}
ItemArchive* Combatant::archive() {
    return present() ? &m_actor.stock->archive : nullptr;
}
} // namespace gdl::game
