#pragma once

#include <algorithm>
#include <optional>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

struct Extent2D {
    u32 width = 0;
    u32 height = 0;

    constexpr bool isZero() const { return width == 0 || height == 0; }
    bool operator==(const Extent2D&) const = default;
};

/** Chooses a supported MSAA count no greater than requested. The mask's bits are
 * the sample counts themselves (1, 2, 4); unsupported requests disable MSAA. */
constexpr u32 presentationSamples(u32 requested, u32 supported) {
    if (requested != 2 && requested != 4) {
        return 1;
    }
    for (u32 count = requested; count > 1; count /= 2) {
        if ((supported & count) != 0) {
            return count;
        }
    }
    return 1;
}

enum class PrimitiveTopology : u8 {
    TriangleList,
    TriangleStrip,
    TriangleFan,
    QuadList,
};

/** Vertex layout for immediate-mode drawing: position, RGBA8 colour, one texcoord. */
struct ImmediateVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Color color = Color::white();
    Vec2 uv{0.0f, 0.0f};
    Vec2 uv2{0.0f, 0.0f}; ///< into the lightmap, when the draw has one

    bool operator==(const ImmediateVertex&) const = default;
};

static_assert(sizeof(ImmediateVertex) == 32);

enum class TextureFilter : u8 { Nearest, Linear };
enum class TextureWrap : u8 { Repeat, ClampToEdge };

/** Number of levels in a complete chain, including the original image. */
constexpr u32 textureMipCount(u32 width, u32 height) {
    u32 count = 1;
    while (width > 1 || height > 1) {
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
        ++count;
    }
    return count;
}

constexpr Extent2D textureMipExtent(Extent2D size, u32 level) {
    while (level-- > 0) {
        size = {std::max(1U, size.width / 2), std::max(1U, size.height / 2)};
    }
    return size;
}

/** Packed RGBA8 byte count; each level immediately follows the preceding one. */
constexpr u64 textureMipBytes(Extent2D size, u32 levels) {
    u64 bytes = 0;
    for (u32 level = 0; level < levels; ++level) {
        bytes += u64{size.width} * size.height * 4;
        size = textureMipExtent(size, 1);
    }
    return bytes;
}

/** 0 preserves base-level sampling; 1 is trilinear; 2/4/8/16 request anisotropy. */
constexpr u32 validTextureFiltering(u32 value) {
    return value == 0 || value == 1 || value == 2 || value == 4 || value == 8 || value == 16 ? value
                                                                                             : 1;
}

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    TextureFilter filter = TextureFilter::Linear;
    TextureWrap wrap = TextureWrap::Repeat;          ///< across (u), and down too unless wrapV says
    std::optional<TextureWrap> wrapV = std::nullopt; ///< down (v), when it differs from across
    u32 mipLevels = 1;            ///< supplied RGBA8 levels, including the base image
    bool generateMipmaps = false; ///< complete any missing tail after the supplied levels

    TextureWrap wrapDown() const { return wrapV.value_or(wrap); }
};

/** GPU texture created by RenderDevice::createTexture. */
class Texture {
public:
    virtual ~Texture() = default;

    GDL_NON_COPYABLE_NON_MOVABLE(Texture);

    virtual u32 width() const = 0;
    virtual u32 height() const = 0;

protected:
    Texture() = default;
};

} // namespace gdl
