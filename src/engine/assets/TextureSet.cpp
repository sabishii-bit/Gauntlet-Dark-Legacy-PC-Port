#include "engine/assets/TextureSet.h"

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/assets/PngImage.h"
#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kManifestName = "textures.json";

} // namespace

bool TextureSet::load(const std::filesystem::path& directory) {
    releaseTextures();
    m_entries.clear();
    m_byName.clear();
    m_images.clear();
    m_directory = directory;

    const std::filesystem::path manifest = directory / kManifestName;
    std::vector<u8> bytes;
    try {
        bytes = readFile(manifest);
    } catch (const std::exception& e) {
        log::warn("Texture set {}: {}", manifest.string(), e.what());
        return false;
    }

    try {
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const nlohmann::json& bitmap : root.at("bitmaps")) {
            TextureSetEntry entry;
            entry.name = normalizeAssetName(bitmap.at("name").get<std::string>());
            entry.width = bitmap.at("width").get<u32>();
            entry.height = bitmap.at("height").get<u32>();
            entry.flags = bitmap.value("flags", 0U);
            entry.frames = bitmap.value("frames", 0U);
            entry.halfResolution = bitmap.value("halfResolution", false);
            entry.clampU = bitmap.value("clampU", false);
            entry.clampV = bitmap.value("clampV", false);
            entry.file = directory / bitmap.at("file").get<std::string>();
            m_entries.push_back(std::move(entry));
        }
        for (u32 i = 0; i < m_entries.size(); ++i) {
            m_byName.try_emplace(m_entries[i].name, i);
        }
        if (root.contains("defs")) {
            for (const nlohmann::json& def : root.at("defs")) {
                const auto index = def.at("index").get<u32>();
                if (index < m_entries.size()) {
                    m_byName[normalizeAssetName(def.at("name").get<std::string>())] = index;
                }
            }
        }
    } catch (const std::exception& e) {
        log::warn("Texture set {}: {}", manifest.string(), e.what());
        m_entries.clear();
        m_byName.clear();
        return false;
    }

    m_images.resize(m_entries.size());
    m_textures.resize(m_entries.size());
    return !m_entries.empty();
}

const TextureSetEntry& TextureSet::entry(u32 index) const {
    GDL_VERIFY(index < m_entries.size(), "texture index out of range");
    return m_entries[index];
}

std::optional<u32> TextureSet::find(std::string_view name) const {
    const auto it = m_byName.find(normalizeAssetName(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

const Image& TextureSet::image(u32 index) {
    GDL_VERIFY(index < m_entries.size(), "texture index out of range");
    Image& image = m_images[index];
    if (image.pixels.empty()) {
        image = loadImageFile(m_entries[index].file);
    }
    return image;
}

const Texture& TextureSet::texture(RenderDevice& device, u32 index) {
    GDL_VERIFY(index < m_entries.size(), "texture index out of range");
    std::unique_ptr<Texture>& texture = m_textures[index];
    if (!texture) {
        const Image& pixels = image(index);
        // Each way wraps or clamps on its own: ground may tile across and not down.
        const auto wrapOf = [](bool clamp) {
            return clamp ? TextureWrap::ClampToEdge : TextureWrap::Repeat;
        };
        texture = device.createTexture(
            TextureDesc{pixels.width, pixels.height, TextureFilter::Linear,
                        wrapOf(m_entries[index].clampU), wrapOf(m_entries[index].clampV)},
            pixels.pixels);
    }
    return *texture;
}

void TextureSet::releaseTextures() {
    for (std::unique_ptr<Texture>& texture : m_textures) {
        texture.reset();
    }
}

} // namespace gdl
