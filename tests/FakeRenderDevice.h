#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::test {

class FakeTexture final : public Texture {
public:
    FakeTexture(u32 width, u32 height) : m_width(width), m_height(height) {}

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }

    std::vector<u8> pixels;

private:
    u32 m_width;
    u32 m_height;
};

struct RecordedDraw {
    const Texture* texture = nullptr;
    std::vector<ImmediateVertex> vertices;
    Mat4 transform{1.0f};
    DrawState state;
    /** Shorthands for the state's blend and lightmap. */
    BlendMode blend() const { return state.blend; }
    const Texture* lightmap() const { return state.lightmap; }
};

/** A RenderDevice that records what is drawn instead of touching a GPU. */
class FakeRenderDevice final : public RenderDevice {
public:
    bool beginFrame() override {
        ++frames;
        return true;
    }
    void endFrame() override {}
    void setClearColor(const Vec4& /*rgba*/) override {}
    Extent2D framebufferExtent() const override { return Extent2D{640, 448}; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> rgba8Pixels) override {
        ++texturesCreated;
        lastTextureDesc = desc;
        auto texture = std::make_unique<FakeTexture>(desc.width, desc.height);
        texture->pixels.assign(rgba8Pixels.begin(), rgba8Pixels.end());
        return texture;
    }

    void updateTexture(Texture& /*texture*/, std::span<const u8> /*rgba8Pixels*/) override {
        ++textureUpdates;
    }

    const Texture& whiteTexture() const override { return m_white; }

    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state) override {
        RecordedDraw record;
        record.texture = &texture;
        record.vertices.assign(batch.triangles().begin(), batch.triangles().end());
        record.transform = transform;
        record.state = state;
        draws.push_back(std::move(record));
    }

    void waitIdle() override {}

    std::vector<RecordedDraw> draws;
    u32 frames = 0;
    u32 texturesCreated = 0;
    u32 textureUpdates = 0;
    TextureDesc lastTextureDesc;

private:
    FakeTexture m_white{1, 1};
};

/** Smallest x and y over a recorded draw's vertices. */
inline Vec2 minCorner(const RecordedDraw& draw) {
    Vec2 corner{1e9f, 1e9f};
    for (const ImmediateVertex& v : draw.vertices) {
        corner.x = std::min(corner.x, v.position.x);
        corner.y = std::min(corner.y, v.position.y);
    }
    return corner;
}

/** Largest x and y over a recorded draw's vertices. */
inline Vec2 maxCorner(const RecordedDraw& draw) {
    Vec2 corner{-1e9f, -1e9f};
    for (const ImmediateVertex& v : draw.vertices) {
        corner.x = std::max(corner.x, v.position.x);
        corner.y = std::max(corner.y, v.position.y);
    }
    return corner;
}

} // namespace gdl::test
