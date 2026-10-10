#include "game/enemies/CombatantAssets.h"

#include <cmath>
#include <format>
#include <set>
#include <string_view>
#include <utility>

#include "engine/assets/TextureBindings.h"
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
    meterArchive.reset();
    meterTree = nullptr;
    meterFill = -1;
    shadow.clear();
    textures.clear();
    skins.clear();
    body.clear();
    attachments.clear();
    brokenModels.clear();
    replacementModels.clear();
    tree = nullptr;
    archive.clear();
    effectLifetimes.clear();
}
const TreeModel* CombatantAssets::prepareReplacement(RenderDevice& device, std::string_view node,
                                                     std::string_view object) {
    const auto index =
        tree != nullptr ? tree->findNode(node, kCombatantNodeNameLength) : std::nullopt;
    if (!index || !archive.models.find(object)) {
        return nullptr;
    }
    const auto key = std::pair{std::string(node), std::string(object)};
    if (const auto found = replacementModels.find(key); found != replacementModels.end()) {
        return &found->second;
    }
    TreeInfo replacement;
    TreeNodeInfo mesh;
    mesh.name = node;
    mesh.object = object;
    mesh.objectFlags = tree->nodes[*index].objectFlags;
    replacement.nodes.push_back(mesh);
    TreeModel model;
    if (!model.bind(replacement, archive.models, archive.textures, device)) {
        return nullptr;
    }
    return &replacementModels.emplace(key, std::move(model)).first->second;
}
bool CombatantAssets::load(RenderDevice& device, const std::filesystem::path& root,
                           const CombatantDefinition& family, char realm,
                           std::span<TextureSet* const> textureLenders) {
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
            part.parentIndex() != 0 ||
            !tree->findNode(part.rootNode(), kCombatantNodeNameLength).has_value()) {
            log::warn("combatant {}: invalid child type {}", definition.name, child);
            clear();
            return false;
        }
        child = part.childIndex();
        children.push_back(std::move(part));
    }
    if (!body.bind(*tree, archive.models, archive.textures, device, textureLenders)) {
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
            !attachment.model.bind(*attachment.tree, archive.models, archive.textures, device,
                                   textureLenders)) {
            clear();
            return false;
        }
        attachments.push_back(std::move(attachment));
    }
    textures.bind(archive.trees.textureAnimations(), archive.textures, device, textureLenders);
    const TextureBindings bindings(archive.textures, textureLenders);
    const auto bindSkin = [&](const std::string& name, TextureBinding first, f64 count) {
        // SFXX life is a frame count at 30 Hz, not a texture's animation flag.
        // A one-frame skin (Yeti's death) is a plain bitmap with frameCount zero.
        count = std::trunc(count);
        if (!std::isfinite(count) || count < 1 || first.index >= first.set->size() ||
            count > static_cast<f64>(first.set->size() - first.index)) {
            log::warn("combatant {}: invalid skin run {}", data.name(), name);
            return;
        }
        std::vector<const Texture*> frames;
        for (u32 frame = 0; frame < static_cast<u32>(count); ++frame) {
            const auto slot = first.index + frame;
            if (first.set->entry(slot).external() || first.set->entry(slot).noPicture) {
                log::warn("combatant {}: missing skin frame {}:{}", data.name(), name, frame);
                return;
            }
            frames.push_back(&first.set->texture(device, slot));
        }
        skins.emplace(name, std::move(frames));
    };
    for (const auto& animation : archive.trees.textureAnimations()) {
        if (!animation.cycles() || animation.frames <= 0 || skins.contains(animation.name)) {
            continue;
        }
        const auto first = animation.source >= 0
                               ? std::optional{TextureBinding{&archive.textures,
                                                              static_cast<u32>(animation.source)}}
                               : bindings.image(animation.frameName);
        if (first) {
            bindSkin(animation.name, *first, animation.frames);
        }
    }
    // CritterInitSfx (0x8003FF98) resolves TEXMOD first, then a named bitmap in
    // the owning model or an already-loaded model. Restrict the latter to the
    // explicitly borrowed stage context; never search unrelated level archives.
    for (const auto& effect : data.sounds()) {
        if ((effect.flags & CombatEffectDefinition::kSkin) == 0 || skins.contains(effect.tree)) {
            continue;
        }
        if (const auto first = bindings.image(effect.tree)) {
            bindSkin(effect.tree, *first, effect.life * 30.0f);
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
            if (!brokenModels[object].bind(replacement, archive.models, archive.textures, device,
                                           textureLenders)) {
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
    if (data.meter().inWorld || definition.healthBar) {
        ItemArchive* source = &archive;
        if (definition.healthBar && !archive.trees.find(kMeterTree)) {
            meterArchive = std::make_unique<ItemArchive>();
            if (meterArchive->load(root / "MONSTERS/GOLEM/LEVELG")) {
                source = meterArchive.get();
            }
        }
        if (const auto found = source->trees.find(kMeterTree); found.has_value()) {
            meterTree = &source->trees.tree(*found);
            if (!meter.bind(*meterTree, source->models, source->textures, device, textureLenders)) {
                meterTree = nullptr;
            } else if (const auto fill = meterTree->findNode(kMeterFill); fill.has_value()) {
                meterFill = static_cast<s32>(*fill);
            }
        }
    }
    return true;
}
} // namespace gdl::game
