#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "formats/ModelArchive.h"

namespace gdl {

struct TextureSetEntry {
    std::string name;
    u32 width = 0;
    u32 height = 0;
    u32 flags = 0;
    u32 frames = 0; ///< frames of the animation starting here (consecutive entries), 0 = none
    bool halfResolution = false; ///< drawn at twice its pixel size
    bool clampU = false;         ///< sampled without wrapping across, so edges do not bleed
    bool clampV = false;
    bool noPicture = false; ///< the archive keeps none for it: it is drawn clear         ///< and
                            ///< down; a texture may tile one way only
    std::filesystem::path file;

    static constexpr u32 kExternal = 0x20; ///< the flag of textures another archive holds
    static constexpr u32 kHasAlpha = 0x80; ///< the flag of textures drawn blended

    /** Whether geometry with this texture is translucent and must draw after the opaque. */
    bool translucent() const { return (flags & kHasAlpha) != 0; }
    /** Whether the image is another archive's, found there by name. */
    bool external() const { return (flags & kExternal) != 0; }
};

/**
 * One texture archive: its named images, decoded and uploaded on
 * first use. Names match ignoring case; an animation's frames follow its first entry.
 */
// NOLINTNEXTLINE(bugprone-exception-escape): the map's move is not noexcept on every library
class TextureSet {
public:
    /** Prefers objects.ngc/textures.ngc; accepts textures.json for inspection exports. */
    bool load(const std::filesystem::path& directory);
    /** Native adapter input. Empty pixels require an explicit external/no-picture slot. */
    static TextureSet fromDecoded(std::vector<TextureSetEntry> entries, std::vector<Image> images);

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const std::filesystem::path& directory() const { return m_directory; }
    const TextureSetEntry& entry(u32 index) const;
    std::optional<u32> find(std::string_view name) const;

    /** Decoded pixels; throws FileError or FormatError when the image cannot be read. */
    const Image& image(u32 index);

    /** GPU texture, created on first use; throws like image(). */
    const Texture& texture(RenderDevice& device, u32 index);
    /** The shape of a native indexed texture whose palette shows nothing (see
     * decodeGcSilhouette): white where its index is set, clear elsewhere. Null for a
     * texture that draws, or that is not native. */
    const Image* silhouette(u32 index);
    /** The silhouette as a clamped GPU texture, created on first use; null without one. */
    const Texture* silhouetteTexture(RenderDevice& device, u32 index);
    /** Already-created native slot only; no decode, cache population or GPU work. */
    const Texture* cachedTexture(u32 index) const {
        return index < m_textures.size() ? m_textures[index].get() : nullptr;
    }
    /** Already-decoded pixels only; native binding inspection must not decode. */
    const Image* cachedImage(u32 index) const {
        return index < m_images.size() && !m_images[index].pixels.empty() ? &m_images[index]
                                                                          : nullptr;
    }

    /** Drops the GPU textures; call before the device goes away. */
    void releaseTextures();

private:
    bool loadNative(const std::filesystem::path& directory, const std::filesystem::path& objects);
    std::vector<formats::ArchiveBitmap> m_nativeBitmaps;
    std::vector<u8> m_nativePixels;
    std::filesystem::path m_directory;
    std::vector<TextureSetEntry> m_entries;
    std::unordered_map<std::string, u32> m_byName;
    std::vector<Image> m_images;
    std::vector<std::unique_ptr<Texture>> m_textures;
    std::vector<std::optional<Image>> m_silhouettes;
    std::vector<bool> m_silhouetteKnown;
    std::vector<std::unique_ptr<Texture>> m_silhouetteTextures;
};

} // namespace gdl
