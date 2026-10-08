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
constexpr f32 kShadowReach = 1.0f; ///< its shadow finds the floor within this of the anchor
constexpr u32 kMeterFacing = 2;    ///< MBTreeSetFlags 0x02000000: turned about the upright
} // namespace
TreePose Combatant::smoothPose(const Actor& actor) {
    if (actor.stock == nullptr || !actor.player.playing()) {
        return actor.pose;
    }
    TreePose pose;
    pose.evaluate(*actor.stock->tree, actor.player.sequence(), actor.player.presentationFrame(),
                  false, true);
    TreePose sampled;
    sampled.evaluate(*actor.stock->tree, actor.player.sequence(), actor.player.frame());
    // Look nodes, held broken parts and linked-parent poses are gameplay-owned
    // overrides. Preserve those rather than resampling over their decisions.
    const auto native = sampled.poses();
    const auto actual = actor.pose.poses();
    for (usize i = 0; i < std::min(native.size(), actual.size()); ++i) {
        if (native[i].rotation != actual[i].rotation || native[i].position != actual[i].position ||
            native[i].scale != actual[i].scale ||
            native[i].pitchYawRoll != actual[i].pitchYawRoll) {
            pose.setNodePose(i, actual[i]);
        }
    }
    return pose;
}

void Combatant::capturePresentation() {
    const auto capture = [](Actor& actor) {
        actor.presentation = {actor.state != State::Inactive,
                              actor.state,
                              actor.player.generation(),
                              actor.player.sequence(),
                              actor.position,
                              actor.yaw,
                              actor.player.presentationFrame(),
                              actor.smoothValid ? actor.smooth : smoothPose(actor)};
        for (auto& attachment : actor.attachments) {
            attachment.previousFrame = attachment.player.presentationFrame();
            attachment.previousGeneration = attachment.player.generation();
        }
    };
    capture(m_actor);
    for (auto& child : m_children) {
        capture(child->m_actor);
    }
}

f32 Combatant::presentationBlend(const Actor& actor, f32 alpha) {
    const auto& before = actor.presentation;
    if (alpha < 0 || !before.valid || before.state != actor.state ||
        before.generation != actor.player.generation() ||
        before.sequence != actor.player.sequence() || actor.definition == nullptr ||
        glm::distance(before.position, actor.position) > 2 * actor.definition->radius()) {
        return 1;
    }
    return std::clamp(alpha, 0.0f, 1.0f);
}

TreePose Combatant::presentationPose(const Actor& actor, f32 alpha) {
    if (alpha < 0 || !actor.presentation.valid) {
        return actor.pose;
    }
    TreePose pose = actor.smoothValid ? actor.smooth : smoothPose(actor);
    pose.blend(actor.presentation.pose, presentationBlend(actor, alpha), true);
    return pose;
}

f32 Combatant::presentationFrame(const Actor& actor, f32 alpha) {
    return std::lerp(actor.presentation.frame, actor.player.presentationFrame(),
                     presentationBlend(actor, alpha));
}

Mat4 Combatant::presentationModel(const Actor& actor, f32 alpha) {
    const f32 blend = presentationBlend(actor, alpha);
    if (blend == 1) {
        return modelTransform(actor);
    }
    const Vec3 position = glm::mix(actor.presentation.position, actor.position, blend);
    const f32 yaw =
        actor.presentation.yaw + TreePose::wrapAngle(actor.yaw - actor.presentation.yaw) * blend;
    const Vec3 root = position + Vec3{0, actor.definition->floorOffset(), 0};
    return glm::scale(glm::rotate(glm::translate(Mat4{1}, root), yaw, Vec3{0, 1, 0}),
                      Vec3{actor.scale * actor.shrink});
}

Mat4 Combatant::modelTransform(const Actor& critter) {
    // The original places the root at floor Y + floorOffset, then transforms originOffset
    // and the animated nodes from that root. Keep our floor probes at the ground anchor:
    // the Dragon's root is 18.5 units above it, outside the probe's vertical search range.
    const Vec3 root = critter.position + Vec3{0.0f, critter.definition->floorOffset(), 0.0f};
    const Mat4 model =
        glm::rotate(glm::translate(Mat4{1.0f}, root), critter.yaw, Vec3{0.0f, 1.0f, 0.0f});
    return glm::scale(model, Vec3{critter.scale * critter.shrink});
}

Vec3 Combatant::partPosition(const Actor& critter, std::string_view node) {
    return Vec3{partTransform(critter, node)[3]};
}

Mat4 Combatant::partTransform(const Actor& critter, std::string_view node) {
    if (node.empty() ||
        !critter.stock->tree->findNode(node, kCombatantNodeNameLength).has_value()) {
        return glm::translate(modelTransform(critter), critter.definition->originOffset());
    }
    return attachmentTransform(critter, node);
}

Mat4 Combatant::attachmentTransform(const Actor& critter, std::string_view node) {
    const Mat4 model = modelTransform(critter);
    if (!node.empty()) {
        if (const auto index = critter.stock->tree->findNode(node, kCombatantNodeNameLength);
            index.has_value()) {
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
        if (const auto index = m_actor.stock->tree->findNode(node, kCombatantNodeNameLength)) {
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
    // CritterMoveNodeCol (GC 0x800374fc) retains a root's movement body while
    // its DEATH sequence runs; CritterDelInst removes it at the final pose.
    // It is no longer a damage target. Dead child branches stay unavailable,
    // including the persistent stump effects retained under a living parent.
    if (!present() || (!alive() && (!solidOnly || m_actor.parent != nullptr))) {
        return out;
    }
    const auto& actor = m_actor;
    for (usize i = 0; i < data()->parts().size(); ++i) {
        const auto& part = data()->parts()[i];
        if (part.radius <= 0 || (solidOnly && (part.flags & CritterPart::kSolid) == 0) ||
            !nodeAvailable(actor, part.node) || actor.hitNodes[i].health <= 0) {
            continue;
        }
        const Vec3 centre{attachmentTransform(actor, part.node) * Vec4{part.position, 1}};
        const f32 radius = part.radius * actor.scale;
        MissileTarget target{id(), centre - Vec3{0, radius, 0}, radius, 2 * radius};
        target.node = static_cast<s32>(i);
        target.targetScoreScale = part.targetScoreScale;
        target.maxTargetDistance = part.maxTargetDistance;
        out.push_back(target);
    }
    if (data()->parts().empty()) {
        out.push_back({id(), position(), radius() * actor.scale, 8 * actor.scale});
    } else if (!actor.branch.has_value()) {
        const Vec3 centre = partPosition(actor, {});
        // The body fallback is wallRadius wide and radius tall on either side
        // of its centre (CritterMoveNodeCol), not a sphere of radius.
        const f32 radius = data()->wallRadius() * actor.scale;
        const f32 halfHeight = data()->radius() * actor.scale;
        out.push_back({id(), centre - Vec3{0, halfHeight, 0}, radius, 2 * halfHeight});
    }
    return out;
}
void Combatant::drawShadow(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                           const WorldLighting& lighting, f32 presentationAlpha) const {
    const Actor& critter = m_actor;
    if (critter.state == State::Inactive || critter.stock == nullptr ||
        !critter.stock->shadow.bound()) {
        return;
    }
    // On the floor under the anchor, tilted to it and scaled with the body (CritterTranslate).
    Vec3 ground = glm::mix(critter.presentation.position, critter.position,
                           presentationBlend(critter, presentationAlpha));
    Vec3 normal{0.0f, 1.0f, 0.0f};
    if (m_collision != nullptr) {
        if (const auto floor = m_collision->floorAt(ground, kShadowReach, kShadowReach)) {
            ground.y = floor->y;
            normal = floor->normal;
        }
    }
    critter.stock->shadow.draw(device, clip, eye, ground, normal, lighting, critter.alpha,
                               critter.scale * critter.shrink);
}

/** The bar hangs from the body's node at the type's offset, turned to the camera about the
 * upright; RED_FILLE is stretched across by the health left of the full (CritterAddHealthMeter,
 * ProcessCritter). It goes with the last of the health. */
std::optional<std::pair<Mat4, std::vector<Mat4>>>
Combatant::meterPose(const CameraFrame* camera, f32 presentationAlpha) const {
    const Actor& critter = m_actor;
    if (critter.state != State::Active || critter.stock == nullptr ||
        critter.stock->meterTree == nullptr || critter.health <= 0.0f ||
        critter.definition == nullptr) {
        return std::nullopt;
    }
    Mat4 placement = glm::translate(presentationModel(critter, presentationAlpha),
                                    critter.definition->meter().barOffset);
    if (camera != nullptr) {
        placement = camera->face(placement, kMeterFacing);
    }
    const TreeInfo& tree = *critter.stock->meterTree;
    const f32 left = std::clamp(critter.health / std::max(critter.maxHealth, 1.0f), 0.0f, 1.0f);
    std::vector<Mat4> matrices(tree.nodes.size(), Mat4{1.0f});
    for (usize n = 0; n < tree.nodes.size(); ++n) {
        const TreeNodeInfo& node = tree.nodes[n];
        Mat4 local = glm::translate(Mat4{1.0f}, node.position);
        if (static_cast<s32>(n) == critter.stock->meterFill) {
            local = glm::scale(local, Vec3{left, 1.0f, 1.0f});
        }
        matrices[n] = node.parent >= 0 && static_cast<usize>(node.parent) < n
                          ? matrices[static_cast<usize>(node.parent)] * local
                          : local;
    }
    return std::pair{placement, std::move(matrices)};
}

void Combatant::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     const Texture* frozenTexture, const CameraFrame* camera,
                     const Texture* hitFlash, f32 presentationAlpha, bool healthBars) const {
    const Actor& critter = m_actor;
    if (critter.state == State::Inactive || critter.stock == nullptr || critter.hidden) {
        return;
    }
    // The model is shared by this species, but object-frame selection belongs to the
    // individual. Set it for every draw, including the first and frozen frames.
    const Mat4 shownModel = presentationModel(critter, presentationAlpha);
    const f32 visualFrame = presentationAlpha >= 0 && critter.presentation.valid
                                ? presentationFrame(critter, presentationAlpha)
                                : critter.player.frame();
    if (presentationAlpha >= 0 && critter.presentation.valid) {
        critter.stock->body.setPresentationFrame(critter.player.sequence(), visualFrame);
    } else {
        critter.stock->body.setFrame(critter.player.sequence(),
                                     static_cast<s32>(critter.player.frame()));
    }
    critter.stock->textures.apply(critter.stock->body, *critter.stock->tree,
                                  critter.player.sequence(), visualFrame,
                                  critter.stock->textures.presentationOffset(presentationAlpha));
    critter.stock->body.setAppearance(false, critter.tint);
    if (critter.skin != nullptr) {
        const auto& skin = *critter.skin;
        const s32 count = static_cast<s32>(skin.life * 30.0f);
        const s32 frame = static_cast<s32>(critter.skinAge * 30.0f * skin.particleRate);
        const auto found = critter.stock->skins.find(skin.tree);
        if (count > 0 && frame < count * (skin.skinLoops + 1) &&
            found != critter.stock->skins.end() &&
            static_cast<usize>(frame % count) < found->second.size()) {
            critter.stock->body.setMaskedTexture(found->second[static_cast<usize>(frame % count)]);
            critter.stock->body.setAppearance(true, critter.tint);
        }
    }
    if (critter.flashTicks > 0 && hitFlash != nullptr) {
        critter.stock->body.setMaskedTexture(hitFlash);
        critter.stock->body.setAppearance(true, Color::white());
    }
    // Retail flashes the normal skin on bit 3 in the final 180 frozen ticks.
    if (frozenTexture != nullptr && critter.frozenTicks > 0 &&
        (critter.frozenTicks >= kThawBlinkTicks || (critter.frozenTicks & kThawBlinkBit) == 0)) {
        critter.stock->body.setMaskedTexture(frozenTexture);
    }
    TreePose pose = presentationPose(critter, presentationAlpha);
    drawNodeState(critter, hitFlash);
    for (const auto& part : m_children) {
        const Actor& branch = part->m_actor;
        if (branch.branch.has_value()) {
            pose.overlaySubtree(presentationPose(branch, presentationAlpha), *branch.branch);
            critter.stock->body.setSubtreeFrame(*branch.branch, branch.player.sequence(),
                                                static_cast<s32>(branch.player.frame()));
            if (branch.hidden) {
                critter.stock->body.setNodeAlpha(*branch.branch, 0.0f);
            }
            if (branch.flashTicks > 0 && hitFlash != nullptr) {
                critter.stock->body.setNodeMaskedTexture(*branch.branch, hitFlash);
            }
            drawNodeState(branch, hitFlash);
        }
    }
    critter.stock->body.draw(device, clip, shownModel, lighting, pose.matrices(), nullptr,
                             critter.alpha);
    for (usize j = 0; j < critter.attachments.size(); ++j) {
        const auto& instance = critter.attachments[j];
        auto& auxiliary = critter.stock->attachments[j];
        const auto& definition = auxiliary.definition;
        Mat4 parent = shownModel;
        if (const auto node = critter.stock->tree->findNode(definition.node);
            node && *node < pose.size()) {
            parent *= pose.matrices()[*node];
        }
        const Mat4 model =
            definition.follows ? glm::translate(parent, definition.offset) : instance.world;
        f32 frame = instance.player.frame();
        TreePose visualPose;
        const TreePose* attachmentPose = &instance.pose;
        if (presentationAlpha >= 0 && critter.presentation.valid) {
            frame = instance.previousGeneration == instance.player.generation()
                        ? std::lerp(instance.previousFrame, instance.player.presentationFrame(),
                                    presentationBlend(critter, presentationAlpha))
                        : instance.player.presentationFrame();
            visualPose.evaluate(*auxiliary.tree, instance.player.sequence(), frame, false, true);
            attachmentPose = &visualPose;
        }
        critter.stock->textures.apply(
            auxiliary.model, *auxiliary.tree, instance.player.sequence(), frame,
            critter.stock->textures.presentationOffset(presentationAlpha));
        auxiliary.model.setPresentationFrame(instance.player.sequence(), frame);
        // MBTreeSetAlpha propagates the body's fade through attached ADDA
        // children. World-rooted trees (the Plague pool) are outside that tree.
        const f32 alpha = definition.follows ? critter.alpha : 1.0f;
        if (camera != nullptr) {
            const auto transforms = attachmentPose->drawMatrices(model, *camera);
            auxiliary.model.draw(device, clip, Mat4{1}, lighting, transforms, nullptr, alpha);
        } else {
            auxiliary.model.draw(device, clip, model, lighting, attachmentPose->matrices(), nullptr,
                                 alpha);
        }
    }
    const Texture* brokenFrozen =
        critter.frozenTicks > 0 && (critter.frozenTicks >= kThawBlinkTicks ||
                                    (critter.frozenTicks & kThawBlinkBit) == 0)
            ? frozenTexture
            : nullptr;
    drawBrokenModels(critter, device, clip, lighting, brokenFrozen, hitFlash, shownModel, pose);
    for (const auto& part : m_children) {
        drawBrokenModels(part->m_actor, device, clip, lighting, brokenFrozen, hitFlash, shownModel,
                         pose);
    }
    if (const auto meter = healthBars ? meterPose(camera, presentationAlpha) : std::nullopt) {
        critter.stock->meter.draw(device, clip, meter->first, lighting, meter->second, nullptr,
                                  critter.alpha);
    }
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
