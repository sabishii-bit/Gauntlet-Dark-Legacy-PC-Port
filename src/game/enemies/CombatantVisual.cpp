#include <format>

#include "game/enemies/Combatant.h"

namespace gdl::game {
std::vector<CombatantVisual> Combatant::visuals(const CameraFrame* camera, bool healthBars) const {
    std::vector<CombatantVisual> result;
    const Actor& actor = m_actor;
    if (!present() || actor.stock == nullptr || actor.hidden) {
        return result;
    }
    const auto& stock = *actor.stock;
    const auto& tree = *stock.tree;
    const bool frozen =
        actor.frozenTicks > 0 && (actor.frozenTicks >= 180 || (actor.frozenTicks & 8) == 0);
    CombatantVisual body;
    body.stock = &stock;
    body.model = &stock.body;
    body.textureTree = &tree;
    body.part = 1;
    body.placement = modelTransform(actor);
    body.sequence = actor.player.sequence();
    body.frame = actor.player.frame();
    body.textureClock = static_cast<f32>(stock.textures.frame());
    body.alpha = actor.alpha;
    body.tint = actor.tint;
    body.flash = actor.flashTicks > 0;
    body.frozen = frozen;
    if (actor.skin != nullptr) {
        const auto& skin = *actor.skin;
        const s32 count = static_cast<s32>(skin.life * 30);
        const s32 frame = static_cast<s32>(actor.skinAge * 30 * skin.particleRate);
        const auto found = stock.skins.find(skin.tree);
        if (count > 0 && frame < count * (skin.skinLoops + 1) && found != stock.skins.end() &&
            static_cast<usize>(frame % count) < found->second.size()) {
            body.skin = found->second[static_cast<usize>(frame % count)];
            body.unlit = true;
        }
    }
    TreePose pose = actor.pose;
    for (const auto& matrix : pose.matrices()) {
        body.nodes.push_back({matrix, actor.player.generation(), body.sequence,
                              static_cast<f32>(static_cast<s32>(body.frame))});
    }
    const auto branchContains = [&](usize root, usize index) {
        for (s32 at = static_cast<s32>(index); at >= 0;
             at = tree.nodes[static_cast<usize>(at)].parent) {
            if (static_cast<usize>(at) == root) {
                return true;
            }
        }
        return false;
    };
    const auto nodeState = [&](const Actor& owner) {
        for (const auto& replacement : owner.modelReplacements) {
            body.nodes[replacement.node].alpha = 0;
        }
        for (usize n = 0; n < body.nodes.size(); ++n) {
            if (nodeRemoved(owner, n)) {
                body.nodes[n].alpha = 0;
            }
        }
        for (usize i = 0; i < owner.hitNodes.size(); ++i) {
            const auto& hit = owner.hitNodes[i];
            const auto& part = owner.definition->parts()[i];
            const auto node = tree.findNode(part.node, kCombatantNodeNameLength);
            if (!node) {
                continue;
            }
            if (hit.broken && stock.brokenModels.contains(
                                  std::format("{}D{}", owner.definition->prefix(), part.node))) {
                body.nodes[*node].alpha = 0;
            }
            body.nodes[*node].flash |= hit.flashTicks > 0;
        }
    };
    nodeState(actor);
    for (const auto& child : m_children) {
        const auto& branch = child->m_actor;
        if (!branch.branch) {
            continue;
        }
        pose.overlaySubtree(branch.pose, *branch.branch);
        for (usize n = 0; n < body.nodes.size(); ++n) {
            if (branchContains(*branch.branch, n)) {
                auto& node = body.nodes[n];
                node.sequence = branch.player.sequence();
                node.frame = static_cast<f32>(static_cast<s32>(branch.player.frame()));
                node.generation = branch.player.generation();
                if (branch.hidden) {
                    node.alpha = 0;
                }
                node.flash |= branch.flashTicks > 0;
            }
        }
        nodeState(branch);
    }
    for (usize i = 0; i < body.nodes.size(); ++i) {
        body.nodes[i].transform = pose.matrices()[i];
    }
    result.push_back(body);
    for (usize j = 0; j < actor.attachments.size(); ++j) {
        const auto& instance = actor.attachments[j];
        const auto& resource = stock.attachments[j];
        const auto& definition = resource.definition;
        Mat4 parent = body.placement;
        if (const auto node = tree.findNode(definition.node); node && *node < pose.size()) {
            parent *= pose.matrices()[*node];
        }
        CombatantVisual visual;
        visual.stock = &stock;
        visual.model = &resource.model;
        visual.textureTree = resource.tree;
        visual.part = static_cast<u32>(2 + j);
        visual.placement =
            definition.follows ? glm::translate(parent, definition.offset) : instance.world;
        visual.sequence = instance.player.sequence();
        visual.frame = instance.player.frame();
        visual.textureClock = body.textureClock;
        visual.alpha = definition.follows ? actor.alpha : 1;
        const auto matrices = camera != nullptr
                                  ? instance.pose.drawMatrices(visual.placement, *camera)
                                  : std::vector<Mat4>(instance.pose.matrices().begin(),
                                                      instance.pose.matrices().end());
        if (camera != nullptr) {
            visual.placement = Mat4{1};
        }
        for (const auto& matrix : matrices) {
            visual.nodes.push_back(
                {matrix, instance.player.generation(), visual.sequence, visual.frame});
        }
        result.push_back(std::move(visual));
    }
    const auto broken = [&](const Actor& owner, u32 branch) {
        if (owner.hidden) {
            return;
        }
        const auto add = [&](const TreeModel* model, usize node, bool flash, u32 part) {
            CombatantVisual visual;
            visual.stock = &stock;
            visual.model = model;
            visual.part = part;
            visual.placement = body.placement * pose.matrices()[node];
            visual.tint = owner.tint;
            visual.alpha = owner.alpha;
            visual.flash = flash;
            visual.frozen = frozen;
            result.push_back(std::move(visual));
        };
        for (const auto& replacement : owner.modelReplacements) {
            if (nodeRemoved(owner, replacement.node)) {
                continue;
            }
            bool flash = owner.flashTicks > 0;
            for (usize i = 0; i < owner.hitNodes.size(); ++i) {
                flash |= tree.findNode(owner.definition->parts()[i].node,
                                       kCombatantNodeNameLength) == replacement.node &&
                         owner.hitNodes[i].flashTicks > 0;
            }
            add(replacement.resource, replacement.node, flash,
                256 + branch * 512 + static_cast<u32>(replacement.node));
        }
        for (usize i = 0; i < owner.hitNodes.size(); ++i) {
            const auto& part = owner.definition->parts()[i];
            const auto node = tree.findNode(part.node, kCombatantNodeNameLength);
            if (!owner.hitNodes[i].broken || !node || nodeRemoved(owner, *node)) {
                continue;
            }
            const auto model = stock.brokenModels.find(
                std::format("{}D{}", owner.definition->prefix(), part.node));
            if (model != stock.brokenModels.end()) {
                add(&model->second, *node, owner.flashTicks > 0 || owner.hitNodes[i].flashTicks > 0,
                    512 + branch * 512 + static_cast<u32>(i));
            }
        }
    };
    broken(actor, 0);
    for (usize i = 0; i < m_children.size(); ++i) {
        broken(m_children[i]->m_actor, static_cast<u32>(i + 1));
    }
    if (const auto meter = healthBars ? meterPose(camera) : std::nullopt) {
        CombatantVisual visual;
        visual.stock = &stock;
        visual.model = &stock.meter;
        visual.part = 65535;
        visual.placement = meter->first;
        visual.alpha = actor.alpha;
        for (const auto& matrix : meter->second) {
            visual.nodes.push_back({matrix});
        }
        result.push_back(std::move(visual));
    }
    return result;
}
} // namespace gdl::game
