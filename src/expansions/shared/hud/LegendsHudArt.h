#pragma once

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

/** The pictures a Legends HUD look uploaded as it opened, by name: one texture a name, kept
 * until cleared, so a look draws without reading or uploading anything. */
class LegendsHudArt {
public:
    /** Keeps `texture` under `name`, in place of any it held there. */
    void add(std::string name, std::unique_ptr<Texture> texture);
    /** The texture kept under `name`; null for a name it holds none under. */
    const Texture* find(std::string_view name) const;
    usize size() const { return m_textures.size(); }
    void clear() { m_textures.clear(); }

private:
    std::map<std::string, std::unique_ptr<Texture>, std::less<>> m_textures;
};

/** Uploads one of a look's pictures, `width` by `height` RGBA8 texels: filtered, and clamped
 * at its edges as the originals' HUD tiles are, so a part drawn from it never wraps round. */
std::unique_ptr<Texture> uploadHudTexture(RenderDevice& device, u32 width, u32 height,
                                          std::span<const u8> rgba);

} // namespace gdl::game
