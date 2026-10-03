#include "game/enemies/CombatantAssets.h"

#include <format>
#include <set>
#include <string_view>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/world/AnimationPlayer.h"
namespace gdl::game {
namespace {
/** Every instance is made with subtype nought: the first of SHADOW1L1..3L1. */
constexpr std::string_view kShadowObject = "SHADOW1L1";
constexpr std::string_view kMeterTree = "GMETER";
constexpr std::string_view kMeterFill = "RED_FILLE";
} // namespace

CombatantAssets::~CombatantAssets() {
    clear();
}
void CombatantAssets::clear() {
    children.clear();
    meter.clear();
    meterTree = nullptr;
    meterFill = -1;
    shadow.clear();
    textures.clear();
    skins.clear();
    body.clear();
    attachments.clear();
    brokenModels.clear();
    tree = nullptr;
    archive.clear();
    effectLifetimes.clear();
}
bool CombatantAssets::load(RenderDevice& device, const std::filesystem::path& root,
                           const CombatantDefinition& family, char realm) {
    clear();
    definition = family;
    if (definition.name.empty() || !data.load(root / "critter" / (definition.name + ".json"))) {
        return false;
    }
    if (definition.kind == CombatantKind::Unknown || data.kind() != definition.kind) {
        log::warn("combatant {}: descriptor family {} does not match requested family {}",
                  definition.name, static_cast<s32>(data.kind()),
                  static_cast<s32>(definition.kind));
        return false;
    }
    const auto directory =
        definition.realmCostume
            ? root / "MONSTERS" / normalizeAssetName(data.folder()) / std::format("LEVEL{}", realm)
            : root / "MONSTERS" / definition.name;
    if (!archive.load(directory)) {
        return false;
    }
    // Shared effects such as a golem's EXPRING are rendered from WEAPONS.
    // Their damage clock must use that same sequence, not a guessed duration.
    AnimationSet shared;
    const AssetLocator common(root / "WEAPONS");
    if (common.find("anim.ps2") || common.find("animations.json")) {
        shared.load(common.root());
    }
    const auto recordLifetimes = [&](const AnimationSet& animations) {
        for (u32 i = 0; i < animations.size(); ++i) {
            const auto& effect = animations.tree(i);
            f32 life = 1;
            if (!effect.sequences.empty()) {
                const auto& sequence = effect.sequences.front();
                const s32 frames = sequence.frames > 0 ? sequence.frames : 30;
                const s32 rate = sequence.frameRate > 0 ? sequence.frameRate : 30;
                life = static_cast<f32>(frames * rate) * AnimationPlayer::kRateUnit;
            }
            effectLifetimes.emplace(effect.name, life);
        }
    };
    recordLifetimes(archive.trees);
    recordLifetimes(shared); // Own trees win; child combatants may reference either archive.
    const auto index = archive.trees.find(data.tree());
    if (!index.has_value()) {
        log::warn("combatant {}: no tree {}", definition.name, data.tree());
        clear();
        return false;
    }
    tree = &archive.trees.tree(*index);
    std::set<s32> visited{0};
    for (s32 child = data.childIndex(); child >= 0;) {
        CritterData part;
        if (!visited.insert(child).second ||
            !part.load(root / "critter" / (definition.name + ".json"), static_cast<usize>(child)) ||
            part.parentIndex() != 0 || !tree->findNode(part.rootNode()).has_value()) {
            log::warn("combatant {}: invalid child type {}", definition.name, child);
            clear();
            return false;
        }
        child = part.childIndex();
        children.push_back(std::move(part));
    }
    if (!body.bind(*tree, archive.models, archive.textures, device)) {
        clear();
        return false;
    }
    for (const auto& definition : data.attachments()) {
        const auto index = archive.trees.find(definition.tree);
        if (!index.has_value()) {
            log::warn("combatant {}: missing auxiliary tree {}", data.name(), definition.tree);
            clear();
            return false;
        }
        Attachment attachment;
        attachment.definition = definition;
        attachment.tree = &archive.trees.tree(*index);
        if (attachment.tree->sequences.empty() ||
            !attachment.model.bind(*attachment.tree, archive.models, archive.textures, device)) {
            clear();
            return false;
        }
        attachments.push_back(std::move(attachment));
    }
    textures.bind(archive.trees.textureAnimations(), archive.textures, device);
    for (const auto& animation : archive.trees.textureAnimations()) {
        if (!animation.cycles() || animation.source < 0 || skins.contains(animation.name)) {
            continue;
        }
        auto& frames = skins[animation.name];
        for (s32 frame = 0; frame < animation.frames; ++frame) {
            const auto slot = static_cast<u32>(animation.source + frame);
            if (slot >= archive.textures.size()) {
                break;
            }
            frames.push_back(&archive.textures.texture(device, slot));
        }
    }
    const auto loadBroken = [&](const CritterData& partData) {
        for (const auto& part : partData.parts()) {
            if ((part.flags & CritterPart::kBreakable) == 0) {
                continue;
            }
            const std::string object = std::format("{}D{}", partData.prefix(), part.node);
            if (!archive.models.find(object).has_value() || brokenModels.contains(object)) {
                continue;
            }
            TreeInfo replacement;
            TreeNodeInfo node;
            node.name = part.node;
            node.object = object;
            replacement.nodes.push_back(node);
            if (!brokenModels[object].bind(replacement, archive.models, archive.textures, device)) {
                brokenModels.erase(object);
            }
        }
    };
    loadBroken(data);
    for (const auto& child : children) {
        loadBroken(child);
    }
    if (data.shadowed()) {
        shadow.bind(device, archive, kShadowObject);
    }
    if (data.meter().inWorld) {
        if (const auto found = archive.trees.find(kMeterTree); found.has_value()) {
            meterTree = &archive.trees.tree(*found);
            if (!meter.bind(*meterTree, archive.models, archive.textures, device)) {
                meterTree = nullptr;
            } else if (const auto fill = meterTree->findNode(kMeterFill); fill.has_value()) {
                meterFill = static_cast<s32>(*fill);
            }
        }
    }
    return true;
}
} // namespace gdl::game
