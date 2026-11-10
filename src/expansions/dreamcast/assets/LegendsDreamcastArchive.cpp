#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"

namespace gdl::game {
namespace {
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
void normalizeTextureCoordinates(Mesh& mesh, const TextureSet& textures) {
    // Native models retain texel coordinates; shared vertices can have differently
    // sized materials. Preserve a separate normalized vertex for each texture slot.
    std::vector<MeshVertex> vertices;
    std::map<std::tuple<u32, u32, u32>, u32> remap;
    for (auto& part : mesh.parts) {
        if (part.texture >= textures.size()) {
            throw FormatError("Legends fixture texture index is out of range");
        }
        const auto& entry = textures.entry(part.texture);
        const Vec2 dimensions{std::max(1u, entry.width), std::max(1u, entry.height)};
        for (auto& index : part.indices) {
            const auto [found, inserted] = remap.try_emplace(
                std::tuple{index, part.texture, part.lightmap}, static_cast<u32>(vertices.size()));
            if (inserted) {
                if (index >= mesh.vertices.size()) {
                    throw FormatError("Legends fixture mesh index is out of range");
                }
                auto vertex = mesh.vertices[index];
                vertex.uv /= dimensions;
                if (part.rgbLightmap) {
                    const auto& lightmap = textures.entry(part.lightmap);
                    vertex.lightmapUv *= Vec2{lightmap.width, lightmap.height};
                }
                vertices.push_back(vertex);
            }
            index = found->second;
        }
    }
    mesh.vertices = std::move(vertices);
}
} // namespace

std::unique_ptr<ItemArchive> loadLegendsDreamcastArchive(const std::filesystem::path& directory) {
    const AssetLocator files{directory};
    const auto models = files.find("objects.rom");
    if (!models) {
        return {};
    }
    const auto bytes = readFile(*models);
    auto objects = formats::decodeDreamcastModelLibrary(bytes);
    auto result = std::make_unique<ItemArchive>();
    const auto textures = files.find("textures.rom");
    const auto pixels = textures ? readFile(*textures) : std::vector<u8>{};
    auto decodedTextures = formats::decodeLegendsDreamcastTextures(bytes, pixels);
    std::map<u32, u32> lightmapSlots;
    usize lightmapBytes = 0;
    // DrawObject's RGB lightmap pass also applies to world meshes instantiated as
    // interactive figures (falling/sinking floors). Keep those descriptors when
    // adapting the same native archive to TreeModel, rather than lighting it twice.
    for (auto& object : objects) {
        for (usize p = 0; p < object.partLightmaps.size(); ++p) {
            const u32 record = object.partLightmaps[p];
            if (record == 0) {
                continue;
            }
            const auto [slot, inserted] =
                lightmapSlots.try_emplace(record, static_cast<u32>(decodedTextures.size()));
            if (inserted) {
                formats::LegendsMapTexture lightmap;
                lightmap.name = "__LIGHTMAP_" + std::to_string(record);
                lightmap.image = formats::decodeDreamcastLightmap(bytes, pixels, record);
                lightmapBytes += lightmap.image.pixels.size();
                if (lightmapBytes > usize{128} * 1024 * 1024) {
                    throw FormatError("Dreamcast fixture lightmap budget exceeded");
                }
                lightmap.clampU = true;
                lightmap.clampV = true;
                decodedTextures.push_back(std::move(lightmap));
            }
            object.mesh.parts[p].lightmap = slot->second;
            object.mesh.parts[p].rgbLightmap = true;
        }
    }
    result->textures = textureSet(std::move(decodedTextures));
    std::vector<ModelSetEntry> entries;
    std::vector<Mesh> meshes;
    for (auto& object : objects) {
        normalizeTextureCoordinates(object.mesh, result->textures);
        entries.push_back({.name = std::move(object.name), .flags = object.mesh.flags});
        meshes.push_back(std::move(object.mesh));
    }
    result->models = ModelSet::fromDecoded(std::move(entries), std::move(meshes));
    if (const auto animation = files.find("anim.rom")) {
        result->trees =
            AnimationSet::fromDecoded(formats::parseDreamcastAnimation(readFile(*animation)));
    }
    return result;
}
} // namespace gdl::game
