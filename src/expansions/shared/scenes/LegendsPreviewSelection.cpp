#include <cmath>

#include "engine/world/SelectionHighlight.h"

#include "expansions/shared/scenes/LegendsPreview.h"

namespace gdl::game {
namespace {
std::optional<f32> intersect(const Vec3& origin, const Vec3& direction, const Vec3& a,
                             const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 p = glm::cross(direction, ac);
    const f32 determinant = glm::dot(ab, p);
    if (std::abs(determinant) < 1e-7f) {
        return {};
    }
    const Vec3 s = origin - a;
    const f32 u = glm::dot(s, p) / determinant;
    const Vec3 q = glm::cross(s, ab);
    const f32 v = glm::dot(direction, q) / determinant;
    const f32 distance = glm::dot(ac, q) / determinant;
    if (u < 0 || v < 0 || u + v > 1 || distance < 0 || !std::isfinite(distance)) {
        return {};
    }
    return distance;
}
} // namespace

bool LegendsPreview::batchVisible(const Batch& batch) const {
    return !batch.presentation.hidden &&
           (!m_adapter || m_adapter->worldObjectVisible(batch.object));
}

Mat4 LegendsPreview::batchPlacement(const Batch& batch, const WorldCamera& view, f32 alpha) const {
    const Mat4 moving = m_adapter ? m_adapter->worldObjectTransform(batch.object, alpha) : Mat4{1};
    if (batch.dreamcastFacing != 0) {
        return moving * CameraFrame::of(view).face(batch.authoredPlacement, batch.dreamcastFacing);
    }
    return batch.presentation.facing == 1
               ? moving * batch.presentation.placement(batch.authoredPlacement, view) *
                     batch.inversePlacement
               : moving;
}

std::optional<ImportedWorldObject> LegendsPreview::object(s32 id) const {
    if (m_adapter && id >= LegendsSceneAdapter::kFixtureObjectBase) {
        return m_adapter->fixtureObject(id);
    }
    if (id < 0 || static_cast<usize>(id) >= m_objects.size()) {
        return {};
    }
    auto result = m_objects[static_cast<usize>(id)];
    result.visible = false;
    for (const auto& batch : m_batches) {
        result.visible |= batch.object == static_cast<usize>(id) && batchVisible(batch);
    }
    if (m_adapter) {
        result.position += m_adapter->worldObjectOffset(static_cast<usize>(id), -1);
    }
    return result;
}

std::optional<ImportedObjectHit> LegendsPreview::pickObject(const Vec3& origin,
                                                            const Vec3& direction,
                                                            const WorldCamera& view,
                                                            f32 alpha) const {
    if (!std::isfinite(glm::dot(origin, origin)) ||
        !std::isfinite(glm::dot(direction, direction)) || glm::length(direction) < 1e-7f) {
        return {};
    }
    const Vec3 ray = glm::normalize(direction);
    std::optional<ImportedObjectHit> nearest;
    for (const auto& batch : m_batches) {
        if (!batchVisible(batch)) {
            continue;
        }
        const Mat4 placement = batchPlacement(batch, view, alpha);
        const auto vertices = batch.geometry.triangles();
        for (usize i = 0; i + 2 < vertices.size(); i += 3) {
            const auto at = [&](usize corner) {
                return Vec3{placement * Vec4{vertices[i + corner].position, 1}};
            };
            if (const auto hit = intersect(origin, ray, at(0), at(1), at(2));
                hit && (!nearest || *hit < nearest->distance)) {
                nearest = ImportedObjectHit{*object(static_cast<s32>(batch.object)), *hit};
            }
        }
    }
    if (m_adapter) {
        const auto fixture = m_adapter->pickFixture(origin, ray, CameraFrame::of(view), alpha);
        if (fixture && (!nearest || fixture->distance < nearest->distance)) {
            nearest = fixture;
        }
    }
    return nearest;
}

void LegendsPreview::highlightObject(RenderDevice& device, const Mat4& clip,
                                     const WorldCamera& view, f32 alpha, s32 id,
                                     const SelectionHighlight& highlight) const {
    if (id < 0) {
        return;
    }
    if (m_adapter && id >= LegendsSceneAdapter::kFixtureObjectBase) {
        m_adapter->highlightFixture(device, clip, CameraFrame::of(view), alpha, id, highlight);
        return;
    }
    for (const auto& batch : m_batches) {
        if (batch.object != static_cast<usize>(id) || !batchVisible(batch)) {
            continue;
        }
        // Only the selected object's batches need this temporary inspection mesh.
        // Use the exact normalized UVs and pose used by the native draw adapter.
        const auto adapted =
            m_adapter ? m_adapter->texture(batch.texture) : LegendsSceneAdapter::TextureView{};
        Mesh mesh;
        MeshPart part;
        const auto vertices = batch.geometry.triangles();
        for (usize i = 0; i + 2 < vertices.size(); i += 3) {
            Vec3 normal = glm::cross(vertices[i + 1].position - vertices[i].position,
                                     vertices[i + 2].position - vertices[i].position);
            if (glm::length(normal) > 1e-7f) {
                normal = glm::normalize(normal);
            }
            for (usize corner = 0; corner < 3; ++corner) {
                const auto& vertex = vertices[i + corner];
                part.indices.push_back(static_cast<u32>(mesh.vertices.size()));
                mesh.vertices.push_back({.position = vertex.position,
                                         .normal = normal,
                                         .uv = vertex.uv * adapted.scale + adapted.offset});
            }
        }
        const Texture* texture = adapted.texture;
        if (!texture && batch.texture < m_surfaces.size()) {
            texture = m_surfaces[batch.texture].texture.get();
        }
        highlight.draw(device, clip, mesh, part, batchPlacement(batch, view, alpha),
                       texture ? *texture : device.whiteTexture());
    }
}
} // namespace gdl::game
