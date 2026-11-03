#include "game/level/NativeStateResources.h"

#include <functional>
#include <limits>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"

#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantDefinition.h"
#include "game/critters/CritterData.h"
#include "game/enemies/EnemyMind.h"
#include "game/level/HazardSurfaces.h"

namespace gdl::game {
namespace {

// These are immutable construction inputs, not a simulation mirror or restore
// protocol. Authored resources use archive indices; generated trees have no such
// index, so distinguish their complete construction once when registering them.
void appendTreeConstruction(NativeStateDigest& digest, const TreeInfo& tree) {
    const auto& [name, prefix, nodes, sequences] = tree;
    digest.values(name, prefix);
    digest.wide(nodes.size());
    for (const auto& node : nodes) {
        const auto& [nodeName, object, type, flags, objectFlags, parent, position, particle,
                     direction, textureAnimation, objectFrames] = node;
        digest.values(nodeName, object, type, flags, objectFlags, parent, position, particle,
                      direction, textureAnimation);
        digest.wide(objectFrames.size());
        for (const auto& run : objectFrames) {
            digest.fields<4>(run);
        }
    }
    digest.wide(sequences.size());
    for (const auto& sequence : sequences) {
        const auto& [sequenceName, frames, frameRate, repeats, fixesPosition, flags, textureStart,
                     textureCount, tracks, trackOfNode] = sequence;
        digest.values(sequenceName, frames, frameRate, repeats, fixesPosition, flags, textureStart,
                      textureCount);
        digest.wide(tracks.size());
        for (const auto& track : tracks) {
            digest.fields<4>(track);
        }
        digest.value(trackOfNode);
    }
}

NativeStateDigest identity(std::string_view kind) {
    NativeStateDigest digest;
    digest.string("NativeStateResources.1");
    digest.string(kind);
    return digest;
}

/** What a family definition contributes to its identity: every field but its combat policies.
 * Each policy is a function of the name and kind hashed here; a definition that set one apart
 * from them would need it hashed too, and the identity's schema bumped with it. */
void appendFamily(NativeStateDigest& digest, const CombatantDefinition& family) {
    [[maybe_unused]] const auto& [name, kind, realmCostume, selection, boundsToHome,
                                  projectileEndVisual, knockbackReduction, breaksItems, patrols,
                                  healthBar, dropForm, targeting, seesInvisible, harmScale, shrinks,
                                  shoved, tramplesSwarm, departure] = family;
    digest.values(name, kind, realmCostume, selection, boundsToHome, projectileEndVisual,
                  knockbackReduction, breaksItems, patrols, healthBar, dropForm);
}

} // namespace

usize NativeStateResources::KeyHash::operator()(const Key& key) const {
    // Hash-table placement is private. Neither this hash nor the pointer ever
    // enters the peer-visible digest.
    return key.type.hash_code() ^ (std::hash<const void*>{}(key.address) << 1);
}

bool NativeStateResources::reject() {
    m_good = false;
    return false;
}

bool NativeStateResources::bind(std::type_index type, const void* address, const Identity& value) {
    if (!m_good || address == nullptr) {
        return reject();
    }
    const auto [it, inserted] = m_bindings.emplace(Key{type, address}, value);
    return inserted || it->second == value ? true : reject();
}

const NativeStateResources::ArchiveBinding*
NativeStateResources::archiveBinding(const ItemArchive& archive) {
    const auto found = m_archives.find(&archive);
    if (!m_good || found == m_archives.end() || !archive.loaded()) {
        reject();
        return nullptr;
    }
    return &found->second;
}

bool NativeStateResources::bindTree(const TreeInfo& tree, const Identity& value) {
    if (!bind(typeid(TreeInfo), &tree, value)) {
        return false;
    }
    for (usize i = 0; i < tree.sequences.size(); ++i) {
        auto sequence = identity("sequence");
        sequence.bytes(value);
        sequence.wide(i);
        sequence.string(tree.sequences[i].name);
        if (!bind(typeid(TreeSequenceInfo), &tree.sequences[i], sequence.finish())) {
            return false;
        }
    }
    return true;
}

bool NativeStateResources::registerArchive(const ItemArchive& archive,
                                           std::string_view logicalKey) {
    if (!m_good || logicalKey.empty() || !archive.loaded()) {
        return reject();
    }
    if (const auto found = m_archives.find(&archive); found != m_archives.end()) {
        return found->second.key == logicalKey ? refreshArchive(archive) : reject();
    }
    auto value = identity("archive");
    value.string(logicalKey);
    const auto archiveIdentity = value.finish();
    if (!bind(typeid(ItemArchive), &archive, archiveIdentity)) {
        return false;
    }
    m_archives.emplace(&archive, ArchiveBinding{std::string{logicalKey}, archiveIdentity});
    if (archive.trees.size() > std::numeric_limits<u32>::max()) {
        return reject();
    }
    for (u32 i = 0; i < archive.trees.size(); ++i) {
        const auto& tree = archive.trees.tree(i);
        auto treeIdentity = identity("authored-tree");
        treeIdentity.bytes(archiveIdentity);
        treeIdentity.word(i);
        treeIdentity.values(tree.name, tree.prefix);
        if (!bindTree(tree, treeIdentity.finish())) {
            return false;
        }
    }
    return refreshArchive(archive);
}

bool NativeStateResources::refreshArchive(const ItemArchive& archive) {
    const auto* source = archiveBinding(archive);
    return source ? registerTextures(archive.textures, source->key) : false;
}

bool NativeStateResources::registerAnimations(const AnimationSet& animations,
                                              std::string_view logicalKey) {
    if (!m_good || logicalKey.empty() || !animations.loaded() ||
        animations.size() > std::numeric_limits<u32>::max()) {
        return reject();
    }
    if (const auto known = m_animationSets.find(&animations); known != m_animationSets.end()) {
        return known->second.key == logicalKey ? true : reject();
    }
    auto bank = identity("animation-bank");
    bank.string(logicalKey);
    const auto bankIdentity = bank.finish();
    if (!bind(typeid(AnimationSet), &animations, bankIdentity)) {
        return false;
    }
    for (u32 i = 0; i < animations.size(); ++i) {
        const auto& tree = animations.tree(i);
        auto treeIdentity = identity("action-tree");
        treeIdentity.bytes(bankIdentity);
        treeIdentity.values(i, tree.name, tree.prefix);
        if (!bindTree(tree, treeIdentity.finish())) {
            return false;
        }
    }
    m_animationSets.emplace(&animations, ArchiveBinding{std::string{logicalKey}, bankIdentity});
    return true;
}

bool NativeStateResources::registerTextures(const TextureSet& textures,
                                            std::string_view logicalKey) {
    if (!m_good || logicalKey.empty() || textures.size() > std::numeric_limits<u32>::max()) {
        return reject();
    }
    auto known = m_textureSets.find(&textures);
    if (known == m_textureSets.end()) {
        auto bank = identity("texture-bank");
        bank.string(logicalKey);
        bank.flag(textures.loaded());
        known = m_textureSets
                    .emplace(&textures,
                             TextureBinding{std::string{logicalKey}, bank.finish(),
                                            std::vector<const Texture*>(textures.size(), nullptr),
                                            std::vector<const Image*>(textures.size(), nullptr)})
                    .first;
    }
    if (known->second.key != logicalKey || known->second.slots.size() != textures.size()) {
        return reject();
    }
    if (!bind(typeid(TextureSet), &textures, known->second.identity)) {
        return false;
    }
    for (u32 slot = 0; slot < textures.size(); ++slot) {
        const auto* image = textures.cachedImage(slot);
        auto& previousImage = known->second.images[slot];
        if (image != previousImage) {
            if (previousImage) {
                return reject();
            }
            if (image) {
                auto value = identity("image");
                value.bytes(known->second.identity);
                value.values(slot, textures.entry(slot).name);
                if (!bind(typeid(Image), image, value.finish())) {
                    return false;
                }
                previousImage = image;
            }
        }
        const auto* texture = textures.cachedTexture(slot);
        auto& previous = known->second.slots[slot];
        if (previous == texture) {
            continue;
        }
        if (previous) {
            return reject(); // A registered bank was released or rebound mid-history.
        }
        if (texture) {
            auto value = identity("texture");
            value.bytes(known->second.identity);
            value.word(slot);
            value.string(textures.entry(slot).name);
            if (!bind(typeid(Texture), texture, value.finish())) {
                return false;
            }
            previous = texture;
        }
    }
    return m_good;
}

bool NativeStateResources::refreshTextures(const TextureSet& textures) {
    const auto known = m_textureSets.find(&textures);
    return known != m_textureSets.end() ? registerTextures(textures, known->second.key) : reject();
}

bool NativeStateResources::registerTexture(const Texture& texture, const ItemArchive& archive,
                                           u32 slot) {
    const auto* source = archiveBinding(archive);
    if (!source || slot >= archive.textures.size() ||
        archive.textures.cachedTexture(slot) != &texture) {
        return reject();
    }
    return registerTextures(archive.textures, source->key);
}

bool NativeStateResources::registerBuiltinTexture(const Texture& texture, std::string_view role) {
    if (!m_good || role.empty()) {
        return reject();
    }
    auto value = identity("builtin-texture");
    value.string(role);
    return bind(typeid(Texture), &texture, value.finish());
}

bool NativeStateResources::registerImage(const Image& image, std::string_view role) {
    if (!m_good || role.empty()) {
        return reject();
    }
    if (const auto known = m_images.find(&image); known != m_images.end()) {
        return known->second.key == role ? true : reject();
    }
    auto value = identity("standalone-image");
    value.string(role);
    value.fields<3>(image);
    const auto fingerprint = value.finish();
    if (!bind(typeid(Image), &image, fingerprint)) {
        return false;
    }
    m_images.emplace(&image, ArchiveBinding{std::string{role}, fingerprint});
    return true;
}

bool NativeStateResources::registerModel(const TreeModel& model, const ItemArchive& archive,
                                         std::string_view bindingKey) {
    const auto* source = archiveBinding(archive);
    if (!source || bindingKey.empty()) {
        return reject();
    }
    auto value = identity("model");
    value.bytes(source->identity);
    value.string(bindingKey);
    return bind(typeid(TreeModel), &model, value.finish());
}

bool NativeStateResources::registerBuiltinTree(const TreeInfo& tree, std::string_view role) {
    if (!m_good || role.empty()) {
        return reject();
    }
    auto value = identity("builtin-tree");
    value.string(role);
    appendTreeConstruction(value, tree);
    return bindTree(tree, value.finish());
}

bool NativeStateResources::registerStaticTree(const TreeInfo& tree, const ItemArchive& archive,
                                              u32 modelIndex, u32 objectFlags) {
    const auto* source = archiveBinding(archive);
    if (!source || modelIndex >= archive.models.size() || tree.nodes.size() != 1 ||
        tree.nodes.front().object != archive.models.entry(modelIndex).name ||
        tree.nodes.front().objectFlags != objectFlags) {
        return reject();
    }
    auto value = identity("static-object-tree");
    value.bytes(source->identity);
    value.word(modelIndex);
    value.word(objectFlags);
    appendTreeConstruction(value, tree);
    return bindTree(tree, value.finish());
}

bool NativeStateResources::registerGeneratedTree(const TreeInfo& tree, const ItemArchive& archive,
                                                 std::string_view bindingKey) {
    const auto* source = archiveBinding(archive);
    if (!source || bindingKey.empty()) {
        return reject();
    }
    auto value = identity("generated-tree");
    value.bytes(source->identity);
    value.string(bindingKey);
    appendTreeConstruction(value, tree);
    return bindTree(tree, value.finish());
}

bool NativeStateResources::registerTextureAnimator(const TextureAnimator& animator,
                                                   const ItemArchive& archive,
                                                   std::string_view bindingKey) {
    const auto* source = archiveBinding(archive);
    if (!source || bindingKey.empty()) {
        return reject();
    }
    auto value = identity("texture-animator");
    value.bytes(source->identity);
    value.string(bindingKey);
    return bind(typeid(TextureAnimator), &animator, value.finish());
}

bool NativeStateResources::registerCritter(const CritterData& data, std::string_view wadKey,
                                           u32 typeIndex) {
    if (!m_good || wadKey.empty() || !data.loaded()) {
        return reject();
    }
    auto value = identity("critter");
    value.values(wadKey, typeIndex, data.childIndex(), data.parentIndex(), data.name(),
                 data.folder(), data.prefix(), data.tree(), data.rootNode(), data.kind());
    value.fields<10>(data.rules());
    for (const auto& sound : data.sounds()) {
        value.fields<14>(sound);
    }
    const auto dataIdentity = value.finish();
    if (!bind(typeid(CritterData), &data, dataIdentity)) {
        return false;
    }
    for (usize i = 0; i < data.sounds().size(); ++i) {
        auto effect = identity("critter-effect");
        effect.bytes(dataIdentity);
        effect.wide(i);
        if (!bind(typeid(CombatEffectDefinition), &data.sounds()[i], effect.finish())) {
            return false;
        }
    }
    return true;
}

bool NativeStateResources::registerCombatantAssets(const CombatantAssets& assets,
                                                   std::string_view logicalKey) {
    if (!registerArchive(assets.archive, logicalKey)) {
        return false;
    }
    auto value = identity("combatant-family");
    value.string(logicalKey);
    appendFamily(value, assets.definition);
    value.values(assets.cooldownStartsWithMove, assets.projectileLifetime, assets.wakeDelay,
                 assets.initialMoveBeforeWake);
    if (!bind(typeid(CombatantAssets), &assets, value.finish())) {
        return false;
    }
    const auto wad = "critter/" + assets.definition.name;
    if (!registerCritter(assets.data, wad, 0)) {
        return false;
    }
    s32 type = assets.data.childIndex();
    for (const auto& child : assets.children) {
        if (type < 0 || !registerCritter(child, wad, static_cast<u32>(type))) {
            return reject();
        }
        type = child.childIndex();
    }
    for (const auto& [name, model] : assets.brokenModels) {
        if (!registerModel(model, assets.archive, "broken/" + name)) {
            return false;
        }
    }
    for (const auto& [key, model] : assets.replacementModels) {
        // Length-prefix each component to keep ambiguous concatenations apart.
        const auto role = std::to_string(key.first.size()) + ":" + key.first + key.second;
        if (!registerModel(model, assets.archive, "replacement/" + role)) {
            return false;
        }
    }
    // The meter is borrowed from a separate archive for some families. Register
    // its existing bindings too, without requesting or constructing any art.
    return (!assets.meterArchive || !assets.meterArchive->loaded() ||
            registerArchive(*assets.meterArchive, std::string(logicalKey) + "/meter")) &&
           registerTextureAnimator(assets.textures, assets.archive, "family-textures");
}

bool NativeStateResources::registerCollision(const WorldCollision& collision,
                                             std::string_view stageKey) {
    if (!m_good || stageKey.empty()) {
        return reject();
    }
    auto value = identity("collision");
    value.string(stageKey);
    return bind(typeid(WorldCollision), &collision, value.finish());
}

bool NativeStateResources::registerHazards(const HazardSurfaces& hazards,
                                           std::string_view stageKey) {
    if (!m_good || stageKey.empty()) {
        return reject();
    }
    auto value = identity("hazards");
    value.string(stageKey);
    return bind(typeid(HazardSurfaces), &hazards, value.finish());
}

bool NativeStateResources::registerLayout(const WorldLayout& layout, std::string_view stageKey) {
    if (!m_good || stageKey.empty()) {
        return reject();
    }
    auto value = identity("world-layout");
    value.string(stageKey);
    return bind(typeid(WorldLayout), &layout, value.finish());
}

bool NativeStateResources::registerLookouts(const LookoutRoute& route, std::string_view ownerKey) {
    if (!m_good || ownerKey.empty()) {
        return reject();
    }
    auto value = identity("lookout-route-owner");
    value.string(ownerKey);
    return bind(typeid(LookoutRoute), &route, value.finish());
}

NativeStateDigest::Resources NativeStateResources::resources() const {
    // the digest's owner pointer is never written through
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return {const_cast<NativeStateResources*>(this), append};
}

bool NativeStateResources::append(NativeStateDigest& digest, std::type_index type,
                                  const void* address, void* context) {
    const auto& registry = *static_cast<const NativeStateResources*>(context);
    const auto found = registry.m_bindings.find(Key{type, address});
    if (!registry.m_good || found == registry.m_bindings.end()) {
        return false;
    }
    digest.bytes(found->second);
    return true;
}

void NativeStateResources::clear() {
    m_bindings.clear();
    m_archives.clear();
    m_animationSets.clear();
    m_textureSets.clear();
    m_images.clear();
    m_good = true;
}

} // namespace gdl::game
