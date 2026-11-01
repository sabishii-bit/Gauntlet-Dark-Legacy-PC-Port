#include "expansions/n64/assets/LegendsN64Archive.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/formats/LegendsN64SequenceGroups.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::game {
namespace {
std::vector<u8> optionalFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    return path ? readFile(*path) : std::vector<u8>{};
}

TextureSet textureSet(std::vector<formats::LegendsMapTexture> textures) {
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (auto& texture : textures) {
        TextureSetEntry entry;
        entry.name = std::move(texture.name);
        entry.width = texture.width != 0 ? texture.width : texture.image.width;
        entry.height = texture.height != 0 ? texture.height : texture.image.height;
        entry.clampU = texture.clampU;
        entry.clampV = texture.clampV;
        entry.flags = texture.hasTransparency ? TextureSetEntry::kHasAlpha : 0;
        if (texture.external) {
            entry.flags |= TextureSetEntry::kExternal;
        }
        entry.noPicture = texture.image.pixels.empty() && !texture.external;
        entries.push_back(std::move(entry));
        images.push_back(std::move(texture.image));
    }
    return TextureSet::fromDecoded(std::move(entries), std::move(images));
}

TextureSet n64Textures(const AssetLocator& files, std::span<const u8> bytes,
                       formats::AnimationFile& animation,
                       std::vector<formats::LegendsN64Model>& models,
                       std::vector<std::string>& warnings) {
    auto decoded = formats::decodeLegendsN64Textures(bytes, optionalFile(files, "texdyn.rom"));
    // Archive-load property7 cycles supply these native placeholder slots before
    // any drawable uses them. They are not ordinary cross-archive name references.
    for (const auto& cycle : animation.textureAnimations) {
        if (cycle.flag != -1 || cycle.texture < 0 || cycle.source < 0 || cycle.frames <= 0) {
            continue;
        }
        const auto target = static_cast<usize>(cycle.texture);
        const auto first = static_cast<usize>(cycle.source);
        const auto count = static_cast<usize>(cycle.frames);
        if (target >= decoded.size() || first >= decoded.size() || count > decoded.size() - first) {
            throw FormatError("N64 shared texture cycle is outside its archive");
        }
        const s32 frame = (cycle.start % cycle.frames + cycle.frames) % cycle.frames;
        const auto& source = decoded[first + static_cast<usize>(frame)];
        auto& destination = decoded[target];
        if (destination.external && !source.external && !source.image.pixels.empty()) {
            destination.image = source.image;
            destination.width = source.width;
            destination.height = source.height;
            destination.clampU = source.clampU;
            destination.clampV = source.clampV;
            destination.hasTransparency = source.hasTransparency;
            destination.external = false;
        }
    }
    formats::resolveLegendsN64ColorRamps(models, decoded, animation, &warnings);
    std::vector<formats::LegendsMapTexture> textures;
    textures.reserve(decoded.size());
    for (auto& texture : decoded) {
        textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                            texture.hasTransparency, std::move(texture.name), texture.external,
                            texture.width, texture.height});
    }
    return textureSet(std::move(textures));
}

void normalizeTextureCoordinates(Mesh& mesh, const TextureSet& textures) {
    // Native decoders retain texel coordinates. A shared vertex may belong to
    // materials with different dimensions, so normalize a copy for each slot.
    std::vector<MeshVertex> vertices;
    std::map<std::pair<u32, u32>, u32> remap;
    for (auto& part : mesh.parts) {
        if (part.texture >= textures.size()) {
            throw FormatError("Legends fixture texture index is out of range");
        }
        const auto& entry = textures.entry(part.texture);
        const Vec2 dimensions{std::max(1u, entry.width), std::max(1u, entry.height)};
        for (auto& index : part.indices) {
            const auto key = std::pair{index, part.texture};
            const auto [found, inserted] =
                remap.try_emplace(key, static_cast<u32>(vertices.size()));
            if (inserted) {
                if (index >= mesh.vertices.size()) {
                    throw FormatError("Legends fixture mesh index is out of range");
                }
                auto vertex = mesh.vertices[index];
                vertex.uv /= dimensions;
                vertices.push_back(vertex);
            }
            index = found->second;
        }
    }
    mesh.vertices = std::move(vertices);
}

struct DecodedArchive {
    std::vector<formats::LegendsN64Model> models;
    TextureSet textures;
    formats::AnimationFile animation;
};

std::optional<DecodedArchive> decodeArchive(const std::filesystem::path& directory,
                                            std::vector<std::string>& warnings,
                                            std::span<const u8> program,
                                            const std::filesystem::path& animationDirectory,
                                            bool bakeSequenceTextures) {
    const AssetLocator files{directory};
    const auto models = files.find("objects.wad");
    if (!models) {
        return {};
    }
    const auto bytes = readFile(*models);
    auto objects = formats::decodeLegendsN64Models(bytes);
    formats::AnimationFile animation;
    const AssetLocator animationFiles{animationDirectory.empty() ? directory : animationDirectory};
    if (const auto sequences = animationFiles.find("seq.wad")) {
        auto decoded = formats::decodeLegendsN64Sequences(
            readFile(*sequences), objects, program, optionalFile(animationFiles, "dynseq.rom"),
            bakeSequenceTextures);
        animation = std::move(decoded.animation);
        for (auto& variant : decoded.modelVariants) {
            objects.push_back(std::move(variant));
        }
        for (auto& warning : decoded.warnings) {
            warnings.push_back(paths::utf8(directory.filename()) + ": " + warning);
        }
    }
    auto textures = n64Textures(files, bytes, animation, objects, warnings);
    return DecodedArchive{std::move(objects), std::move(textures), std::move(animation)};
}

void appendShared(DecodedArchive& own, DecodedArchive shared, usize bank) {
    if (!own.animation.particles.empty() || !shared.animation.particles.empty()) {
        throw FormatError("N64 archive composition: unexpected particle templates");
    }
    // Preserve every indexed slot and frame run. Native groups may refer to the
    // same texture name in separate banks; indices never collapse by name.
    const u32 textureBase = static_cast<u32>(own.textures.size());
    const s32 animationBase = static_cast<s32>(own.animation.textureAnimations.size());
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    const auto appendTextures = [&](TextureSet& bank, TextureSet& lender) {
        for (u32 i = 0; i < bank.size(); ++i) {
            auto entry = bank.entry(i);
            const Image* image = bank.cachedImage(i);
            if (entry.external() && !entry.noPicture) {
                if (const auto slot = lender.find(entry.name);
                    slot && !lender.entry(*slot).external() && !lender.entry(*slot).noPicture) {
                    image = lender.cachedImage(*slot);
                    if (image != nullptr) {
                        const auto name = entry.name;
                        entry = lender.entry(*slot);
                        entry.name = name;
                    }
                }
            }
            entries.push_back(std::move(entry));
            images.push_back(image != nullptr ? *image : Image{});
        }
    };
    appendTextures(own.textures, shared.textures);
    appendTextures(shared.textures, own.textures);
    own.textures = TextureSet::fromDecoded(std::move(entries), std::move(images));
    const auto modelName = [bank](const std::string& name) {
        return name.empty() ? name : "@N64SHARED/" + std::to_string(bank) + "/" + name;
    };
    for (auto& model : shared.models) {
        model.name = modelName(model.name);
        for (auto& part : model.mesh.parts) {
            if (part.texture != formats::LegendsN64Model::kRuntimeTexture) {
                part.texture += textureBase;
            }
        }
        own.models.push_back(std::move(model));
    }
    for (auto& animation : shared.animation.textureAnimations) {
        if (animation.texture >= 0)
            animation.texture += static_cast<s32>(textureBase);
        if (animation.source >= 0)
            animation.source += static_cast<s32>(textureBase);
        own.animation.textureAnimations.push_back(std::move(animation));
    }
    std::set<std::string> names;
    for (const auto& tree : own.animation.trees)
        names.insert(normalizeAssetName(tree.name));
    for (auto& tree : shared.animation.trees) {
        if (!names.insert(normalizeAssetName(tree.name)).second)
            continue;
        for (auto& node : tree.nodes) {
            node.object = modelName(node.object);
            for (auto& run : node.objectFrames)
                run.object = modelName(run.object);
            if (node.textureAnimation >= 0)
                node.textureAnimation += animationBase;
        }
        for (auto& sequence : tree.sequences) {
            if (sequence.textureAnimationStart >= 0)
                sequence.textureAnimationStart += animationBase;
        }
        own.animation.trees.push_back(std::move(tree));
    }
}

void exposeSharedDefaultSequences(formats::AnimationFile& animation) {
    // Boss SFXX -> e0076078 requests sequence FOURCC zero, not the first
    // sequence in a group (e002db74 reserves -1 for that). Other native weapon
    // spells have unrelated BODY hierarchies: keep their GROUP:FOURCC identity
    // rather than merging MAGIC or WEAPONS into a fictitious common skeleton.
    for (auto& tree : animation.trees) {
        if (tree.sequences.size() == 1 && tree.sequences.front().name.empty() &&
            !tree.nodes.empty()) {
            const auto colon = tree.name.find(':');
            if (colon == std::string::npos || colon + 1 != tree.name.size()) {
                throw FormatError("N64 shared effect: invalid default sequence identity");
            }
            tree.name.resize(colon);
        }
    }
}
} // namespace

std::unique_ptr<ItemArchive>
loadLegendsN64Archive(const std::filesystem::path& directory, std::vector<std::string>& warnings,
                      std::span<const u8> program, const std::filesystem::path& animationDirectory,
                      bool combinedGroups,
                      std::span<const std::filesystem::path> sharedDirectories,
                      const std::function<void(formats::AnimationFile&)>& translate) {
    auto decoded = decodeArchive(directory, warnings, program, animationDirectory, combinedGroups);
    if (!decoded)
        return {};
    if (combinedGroups)
        formats::combineLegendsN64SequenceGroups(decoded->animation);
    usize bank = 0;
    for (const auto& sharedDirectory : sharedDirectories) {
        // Shared effects keep independent sequence hierarchies, so their keyed
        // texture players need no combatant-style per-sequence model baking.
        auto shared = decodeArchive(sharedDirectory, warnings, program, {}, false);
        if (!shared)
            throw FileError("N64 shared archive is missing");
        if (combinedGroups)
            exposeSharedDefaultSequences(shared->animation);
        appendShared(*decoded, std::move(*shared), bank++);
    }
    auto result = std::make_unique<ItemArchive>();
    result->textures = std::move(decoded->textures);
    std::vector<ModelSetEntry> entries;
    std::vector<Mesh> meshes;
    for (auto& object : decoded->models) {
        // Segment 13 is supplied by the drawable's texture property. Its per-node
        // variant above has the real slot; the original must not become texture zero.
        std::erase_if(object.mesh.parts, [](const MeshPart& part) {
            return part.texture == formats::LegendsN64Model::kRuntimeTexture;
        });
        normalizeTextureCoordinates(object.mesh, result->textures);
        entries.push_back({.name = std::move(object.name)});
        meshes.push_back(std::move(object.mesh));
    }
    result->models = ModelSet::fromDecoded(std::move(entries), std::move(meshes));
    if (translate)
        translate(decoded->animation);
    result->trees = AnimationSet::fromDecoded(std::move(decoded->animation));
    return result;
}

} // namespace gdl::game
