#include "engine/world/WorldScene.h"

#include <algorithm>
#include <exception>
#include <unordered_map>

#include "engine/core/Log.h"

namespace gdl {

namespace {

constexpr u32 kTextureHasAlpha = 0x80;

} // namespace

bool WorldScene::build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
                       RenderDevice& device, const WorldLighting& lighting) {
    clear();
    const Vec3 light = glm::normalize(lighting.direction);
    // Texture index to batch index; kUnusable marks a texture that could not be loaded, so
    // it is reported once and its geometry skipped.
    constexpr auto kUnusable = static_cast<usize>(-1);
    std::unordered_map<u32, usize> batchByTexture;
    const auto batchFor = [&](u32 texture) -> Batch* {
        if (const auto found = batchByTexture.find(texture); found != batchByTexture.end()) {
            return found->second == kUnusable ? nullptr : &m_batches[found->second];
        }
        if (texture >= textures.size()) {
            batchByTexture[texture] = kUnusable;
            return nullptr;
        }
        try {
            Batch batch;
            batch.texture = &textures.texture(device, texture);
            batch.translucent = (textures.entry(texture).flags & kTextureHasAlpha) != 0;
            batch.geometry.begin(PrimitiveTopology::TriangleList);
            m_batches.push_back(std::move(batch));
        } catch (const std::exception& e) {
            log::warn("World scene: texture {} skipped: {}", texture, e.what());
            batchByTexture[texture] = kUnusable;
            return nullptr;
        }
        batchByTexture[texture] = m_batches.size() - 1;
        return &m_batches.back();
    };

    const std::vector<WorldObject>& objects = layout.objects();
    for (usize i = 0; i < objects.size(); ++i) {
        const WorldObject& object = objects[i];
        const auto model = models.find(object.name);
        if (!model.has_value()) {
            continue;
        }
        const Mesh* mesh = nullptr;
        try {
            mesh = &models.mesh(*model);
        } catch (const std::exception& e) {
            log::warn("World scene: {}: {}", object.name, e.what());
            continue;
        }
        const Vec3 offset = layout.worldPosition(i);
        const bool chrome = (object.objectFlags & WorldObject::kChrome) != 0;
        bool placed = false;
        for (const MeshPart& part : mesh->parts) {
            Batch* batch = batchFor(part.texture);
            if (batch == nullptr) {
                continue;
            }
            for (const u32 index : part.indices) {
                const MeshVertex& v = mesh->vertices[index];
                const f32 shade = std::clamp(
                    lighting.ambient + lighting.diffuse * glm::dot(v.normal, light), 0.0f, 1.0f);
                const Vec2 uv =
                    chrome ? Vec2{0.5f * (1.0f - v.normal.x), 0.5f * (1.0f - v.normal.y)} : v.uv;
                batch->geometry.vertex(v.position + offset, Color::fromFloats(shade, shade, shade),
                                       uv);
            }
            m_triangles += part.indices.size() / 3;
            placed = true;
        }
        if (placed) {
            ++m_placed;
        }
    }
    for (Batch& batch : m_batches) {
        batch.geometry.end();
    }
    std::erase_if(m_batches, [](const Batch& batch) { return batch.geometry.empty(); });
    // Opaque first so translucent surfaces blend over what stands behind them.
    std::stable_partition(m_batches.begin(), m_batches.end(),
                          [](const Batch& batch) { return !batch.translucent; });
    if (m_batches.empty()) {
        log::warn("World scene: no placed object has a mesh");
        return false;
    }
    return true;
}

void WorldScene::clear() {
    m_batches.clear();
    m_placed = 0;
    m_triangles = 0;
}

void WorldScene::draw(RenderDevice& device, const Mat4& clip) const {
    for (const Batch& batch : m_batches) {
        device.draw(batch.geometry, *batch.texture, clip);
    }
}

} // namespace gdl
