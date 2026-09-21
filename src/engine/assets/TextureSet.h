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
    bool noPicture = false; ///< the archive keeps none for it: it is drawn clear         ///< and down; a texture may tile one way only
    std::filesystem::path file;

    static constexpr u32 kExternal = 0x20; ///< the flag of textures another archive holds
    static constexpr u32 kHasAlpha = 0x80; ///< the flag of textures drawn blended

    /** Whether geometry with this texture is translucent and must draw after the opaque. */
    bool translucent() const { return (flags & kHasAlpha) != 0; }
    /** Whether the image is another archive's, found there by name. */
    bool external() const { return (flags & kExternal) != 0; }
};

/**
 * One unpacked texture directory: its manifest of named images, decoded and uploaded on
 * first use. Names match ignoring case; an animation's frames follow its first entry.
 */
class TextureSet {
public:
    /** Reads `directory/textures.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const TextureSetEntry& entry(u32 index) const;
    std::optional<u32> find(std::string_view name) const;

    /** Decoded pixels; throws FileError or FormatError when the image cannot be read. */
    const Image& image(u32 index);

    /** GPU texture, created on first use; throws like image(). */
    const Texture& texture(RenderDevice& device, u32 index);

    /** Drops the GPU textures; call before the device goes away. */
    void releaseTextures();

private:
    std::filesystem::path m_directory;
    std::vector<TextureSetEntry> m_entries;
    std::unordered_map<std::string, u32> m_byName;
    std::vector<Image> m_images;
    std::vector<std::unique_ptr<Texture>> m_textures;
};

} // namespace gdl
