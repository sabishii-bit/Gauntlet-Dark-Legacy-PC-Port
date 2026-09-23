#pragma once

#include <cstdint>
#include <optional>

#include "engine/core/SpecialMembers.h"
#include "engine/math/Math.h"

namespace gdl {

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    constexpr bool isZero() const { return width == 0 || height == 0; }
    bool operator==(const Extent2D&) const = default;
};

enum class PrimitiveTopology : std::uint8_t {
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

enum class TextureFilter : std::uint8_t { Nearest, Linear };
enum class TextureWrap : std::uint8_t { Repeat, ClampToEdge };

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFilter filter = TextureFilter::Linear;
    TextureWrap wrap = TextureWrap::Repeat;          ///< across (u), and down too unless wrapV says
    std::optional<TextureWrap> wrapV = std::nullopt; ///< down (v), when it differs from across

    TextureWrap wrapDown() const { return wrapV.value_or(wrap); }
};

/** GPU texture created by RenderDevice::createTexture. */
class Texture {
public:
    virtual ~Texture() = default;

    GDL_NON_COPYABLE_NON_MOVABLE(Texture);

    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;

protected:
    Texture() = default;
};

} // namespace gdl
