#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
bool Combatant::replaceNodeModel(RenderDevice& device, std::string_view node,
                                 std::string_view object) {
    if (!present()) {
        return false;
    }
    Actor& actor = m_actor;
    const auto index = actor.stock->tree->findNode(node, kCombatantNodeNameLength);
    if (!index) {
        return false;
    }
    if (object.empty()) {
        std::erase_if(actor.modelReplacements,
                      [&](const auto& replacement) { return replacement.node == *index; });
        return true;
    }
    if (!actor.stock->archive.models.find(object)) {
        return false;
    }
    TreeInfo tree;
    TreeNodeInfo mesh;
    mesh.name = node;
    mesh.object = object;
    mesh.objectFlags = actor.stock->tree->nodes[*index].objectFlags;
    tree.nodes.push_back(mesh);
    Actor::ModelReplacement replacement;
    replacement.node = *index;
    if (!replacement.model.bind(tree, actor.stock->archive.models, actor.stock->archive.textures,
                                device)) {
        return false;
    }
    std::erase_if(actor.modelReplacements,
                  [&](const auto& previous) { return previous.node == *index; });
    actor.modelReplacements.push_back(std::move(replacement));
    return true;
}

bool Combatant::nodeRemoved(const Actor& actor, usize node) {
    const auto& tree = *actor.stock->tree;
    s32 parent = tree.nodes[node].parent;
    while (parent >= 0) {
        for (usize i = 0; i < actor.hitNodes.size(); ++i) {
            const auto& part = actor.definition->parts()[i];
            if (actor.hitNodes[i].broken && (part.flags & CritterPart::kRemoveChildren) != 0 &&
                tree.findNode(part.node, kCombatantNodeNameLength) == static_cast<u32>(parent)) {
                return true;
            }
        }
        parent = tree.nodes[static_cast<usize>(parent)].parent;
    }
    return false;
}

bool Combatant::nodeAvailable(const Actor& actor, std::string_view name) {
    const auto node = actor.stock->tree->findNode(name, kCombatantNodeNameLength);
    if (!node || nodeRemoved(actor, *node)) {
        return false;
    }
    for (usize i = 0; i < actor.hitNodes.size(); ++i) {
        if (actor.hitNodes[i].broken && actor.definition->parts()[i].node == name) {
            return false;
        }
    }
    return true;
}

void Combatant::holdBrokenPoses(Actor& actor) {
    for (usize i = 0; i < actor.hitNodes.size(); ++i) {
        const auto& heldPose = actor.hitNodes[i].heldPose;
        if (heldPose) {
            if (const auto node = actor.stock->tree->findNode(actor.definition->parts()[i].node,
                                                              kCombatantNodeNameLength)) {
                actor.pose.setNodePose(*node, *heldPose);
            }
        }
    }
}

f32 Combatant::damageNode(s32 index, f32 amount, u32 flags) {
    if (index < 0 || (flags & kFlashesWhole) != 0) {
        return amount;
    }
    const auto at = static_cast<usize>(index);
    Actor& actor = m_actor;
    if (at >= actor.hitNodes.size()) {
        return 0;
    }
    HitNode& state = actor.hitNodes[at];
    const CritterPart& part = data()->parts()[at];
    const auto node = actor.stock->tree->findNode(part.node, kCombatantNodeNameLength);
    if (!node || state.health <= 0 || !nodeAvailable(actor, part.node)) {
        return 0;
    }
    amount *= part.damageScale;
    // CritterDamage breaks only on overrun, not on an exact-budget hit.
    const bool overrun = amount > state.health;
    amount = std::clamp(amount, 0.0f, state.health);
    state.health -= amount;
    if (overrun && (part.flags & CritterPart::kBreakable) != 0) {
        const Vec3 origin{attachmentTransform(actor, part.node) * Vec4{part.position, 1}};
        state.broken = true;
        state.heldPose = actor.pose.poses()[*node];
        if (const AttackDefinition* damage = data()->damage(part.damageEffect)) {
            if (damage->type == AttackDefinition::kProjectile) {
                CombatShot shot;
                shot.data = data();
                shot.critter = id();
                shot.damageIndex = part.damageEffect;
                shot.origin = origin + Vec3{modelTransform(actor) * Vec4{damage->offset, 0}};
                shot.forward = {std::sin(actor.yaw), 0, std::cos(actor.yaw)};
                shot.rate = attackRate(actor);
                shot.scale = actor.scale;
                shot.damageScale = m_scales.damage;
                shot.realm = m_realm;
                m_shots.push_back(shot);
            } else {
                cue(actor, id(), damage->sound, origin);
            }
        }
        if (actor.move >= 0) {
            const auto& move = data()->moves()[static_cast<usize>(actor.move)];
            constexpr u32 kRequiresNode = 0x10;
            if ((move.flags & kRequiresNode) != 0 && !nodeAvailable(actor, move.colnode)) {
                actor.moveDone = true;
            }
        }
    }
    return amount;
}

void Combatant::drawNodeState(const Actor& actor, const Texture* flash) {
    auto& body = actor.stock->body;
    for (const auto& replacement : actor.modelReplacements) {
        body.setMeshAlpha(replacement.node, 0);
    }
    for (usize node = 0; node < actor.stock->tree->nodes.size(); ++node) {
        if (nodeRemoved(actor, node)) {
            body.setMeshAlpha(node, 0);
        }
    }
    for (usize i = 0; i < actor.hitNodes.size(); ++i) {
        const auto& state = actor.hitNodes[i];
        const auto& part = actor.definition->parts()[i];
        const auto node = actor.stock->tree->findNode(part.node, kCombatantNodeNameLength);
        if (!node) {
            continue;
        }
        if (state.broken && actor.stock->brokenModels.contains(
                                std::format("{}D{}", actor.definition->prefix(), part.node))) {
            body.setMeshAlpha(*node, 0);
        }
        if (state.flashTicks > 0 && flash != nullptr) {
            body.setMeshMaskedTexture(*node, flash);
        }
    }
}

void Combatant::drawBrokenModels(const Actor& actor, RenderDevice& device, const Mat4& clip,
                                 const WorldLighting& lighting, const Texture* frozen,
                                 const Texture* flash, const Mat4& placement,
                                 const TreePose& pose) {
    if (actor.hidden) {
        return;
    }
    for (const auto& replacement : actor.modelReplacements) {
        if (nodeRemoved(actor, replacement.node)) {
            continue;
        }
        bool flashing = actor.flashTicks > 0;
        for (usize i = 0; i < actor.hitNodes.size(); ++i) {
            const auto node = actor.stock->tree->findNode(actor.definition->parts()[i].node,
                                                          kCombatantNodeNameLength);
            flashing |= node == replacement.node && actor.hitNodes[i].flashTicks > 0;
        }
        replacement.model.resetTextures();
        replacement.model.setAppearance(flashing && flash != nullptr, actor.tint);
        const Texture* mask = flashing ? flash : nullptr;
        replacement.model.setMaskedTexture(frozen != nullptr ? frozen : mask);
        replacement.model.draw(device, clip, placement * pose.matrices()[replacement.node],
                               lighting, {}, nullptr, actor.alpha);
    }
    for (usize i = 0; i < actor.hitNodes.size(); ++i) {
        const auto& part = actor.definition->parts()[i];
        const auto node = actor.stock->tree->findNode(part.node, kCombatantNodeNameLength);
        if (!actor.hitNodes[i].broken || !node || nodeRemoved(actor, *node)) {
            continue;
        }
        const auto model = actor.stock->brokenModels.find(
            std::format("{}D{}", actor.definition->prefix(), part.node));
        if (model != actor.stock->brokenModels.end()) {
            TreeModel& replacement = model->second;
            replacement.resetTextures();
            const bool flashing = actor.flashTicks > 0 || actor.hitNodes[i].flashTicks > 0;
            replacement.setAppearance(flashing && flash != nullptr, actor.tint);
            const Texture* mask = flashing ? flash : nullptr;
            replacement.setMaskedTexture(frozen != nullptr ? frozen : mask);
            replacement.draw(device, clip, placement * pose.matrices()[*node], lighting, {},
                             nullptr, actor.alpha);
        }
    }
}
} // namespace gdl::game
