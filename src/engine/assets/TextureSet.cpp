#include "engine/assets/TextureSet.h"

#include <exception>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/assets/PngImage.h"
#include "engine/core/Assert.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "formats/GcTexture.h"

namespace gdl {

namespace {

/** The archive's mark on a bitmap that has no picture of its own. */
constexpr u32 kNoPictureFlag = 0x100;

constexpr std::string_view kManifestName = "textures.json";

} // namespace

bool TextureSet::load(const std::filesystem::path& directory) {
    releaseTextures();
    m_entries.clear();
    m_byName.clear();
    m_images.clear();
    m_textures.clear();
    m_nativeBitmaps.clear();
    m_nativePixels.clear();
    m_directory = directory;

    if (const auto file = AssetLocator(directory).find("objects.ngc")) {
        return loadNative(directory, *file);
    }

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
            entry.noPicture = (bitmap.value("flags", 0U) & kNoPictureFlag) != 0;
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
        // An archive marks some slots as having no picture of their own (an animated
        // texture's, filled from frames kept elsewhere): those are clear, so that what wears
        // them is unseen rather than the whole model failing.
        if (!m_entries[index].noPicture) {
            if (!m_nativeBitmaps.empty()) {
                if (m_entries[index].external()) {
                    throw FileError("external texture requires its owning archive: " +
                                    m_entries[index].name);
                }
                image = formats::decodeGcTexture(m_nativeBitmaps[index], m_nativePixels);
                image.bleedIntoTransparent();
            } else {
                image = loadImageFile(m_entries[index].file);
            }
        } else {
            image.width = 1;
            image.height = 1;
            image.pixels.assign(4, 0);
        }
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

bool TextureSet::loadNative(const std::filesystem::path& directory,
                            const std::filesystem::path& objects) {
    try {
        const auto archive = formats::ModelArchive::parse(readFile(objects));
        m_nativeBitmaps = archive.bitmaps();
        // External and animation-only slots need no companion pixel file.
        if (const auto pixels = AssetLocator(directory).find("textures.ngc")) {
            m_nativePixels = readFile(*pixels);
        }
        m_entries.resize(m_nativeBitmaps.size());
        for (const auto& def : archive.bitmapDefs()) {
            if (def.textureIndex >= m_entries.size()) {
                throw FormatError("bitmap definition index out of range");
            }
            const auto name = normalizeAssetName(def.name);
            auto& entry = m_entries[def.textureIndex];
            if (entry.name.empty()) {
                entry.name = name;
            }
            m_byName[name] = def.textureIndex;
        }
        std::string base = "UNNAMED";
        u32 frame = 0;
        for (u32 i = 0; i < m_entries.size(); ++i) {
            auto& entry = m_entries[i];
            const auto& bitmap = m_nativeBitmaps[i];
            if (entry.name.empty()) {
                entry.name = std::format("{}+{}", base, ++frame);
            } else {
                base = entry.name;
                frame = 0;
            }
            entry.width = bitmap.width;
            entry.height = bitmap.height;
            entry.flags = bitmap.flags;
            entry.frames = bitmap.frameCount;
            entry.noPicture = (bitmap.flags & formats::bitmap_flags::kInvalid) != 0;
            entry.halfResolution = (bitmap.flags & formats::bitmap_flags::kHalfResolution) != 0;
            entry.clampU = (bitmap.flags & formats::bitmap_flags::kClampU) != 0;
            entry.clampV = (bitmap.flags & formats::bitmap_flags::kClampV) != 0;
            if (!entry.noPicture && !entry.external() && m_nativePixels.empty()) {
                throw FileError("archive has local textures but no textures.ngc pixels");
            }
            m_byName.try_emplace(entry.name, i);
        }
        m_images.resize(m_entries.size());
        m_textures.resize(m_entries.size());
        return !m_entries.empty();
    } catch (const std::exception& e) {
        log::warn("Native texture set {}: {}", objects.string(), e.what());
        m_entries.clear();
        m_byName.clear();
        m_nativeBitmaps.clear();
        m_nativePixels.clear();
        return false;
    }
}

} // namespace gdl
