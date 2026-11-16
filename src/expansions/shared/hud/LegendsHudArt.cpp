#include "expansions/shared/hud/LegendsHudArt.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

void LegendsHudArt::add(std::string name, std::unique_ptr<Texture> texture) {
    GDL_VERIFY(texture != nullptr, "LegendsHudArt::add without a texture");
    m_textures.insert_or_assign(std::move(name), std::move(texture));
}

const Texture* LegendsHudArt::find(std::string_view name) const {
    const auto found = m_textures.find(name);
    return found == m_textures.end() ? nullptr : found->second.get();
}

std::unique_ptr<Texture> uploadHudTexture(RenderDevice& device, u32 width, u32 height,
                                          std::span<const u8> rgba) {
    GDL_VERIFY(rgba.size() == usize{width} * height * 4, "uploadHudTexture: size mismatch");
    return device.createTexture({width, height, TextureFilter::Linear, TextureWrap::ClampToEdge},
                                rgba);
}

} // namespace gdl::game
