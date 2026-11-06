#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {
namespace {

bool finite(const Vec3& value) {
    return std::isfinite(glm::dot(value, value));
}

std::optional<f32> triangleHit(const Vec3& origin, const Vec3& direction, const Vec3& a,
                               const Vec3& b, const Vec3& c) {
    if (!finite(a) || !finite(b) || !finite(c)) {
        return {};
    }
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 p = glm::cross(direction, ac);
    const f32 determinant = glm::dot(ab, p);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-7f) {
        return {};
    }
    const Vec3 s = origin - a;
    const f32 u = glm::dot(s, p) / determinant;
    const Vec3 q = glm::cross(s, ab);
    const f32 v = glm::dot(direction, q) / determinant;
    const f32 distance = glm::dot(ac, q) / determinant;
    const bool hits = u >= 0 && v >= 0 && u + v <= 1 && distance >= 0;
    if (!hits || !std::isfinite(distance)) {
        return {};
    }
    return distance;
}

/** Observe the normal CPU draw path instead of maintaining another pose, morph,
 * texture-lender and billboard implementation. Nothing is uploaded or retained.
 * Only a console click/highlight uses this sink; simulation never calls it. */
class FixtureInspection final : public RenderDevice {
public:
    using Draw =
        std::function<void(const ImmediateBatch&, const Texture&, const Mat4&, const DrawState&)>;
    explicit FixtureInspection(Draw draw, const Texture* white = nullptr)
        : m_draw(std::move(draw)), m_white(white ? white : &m_fallback) {}

    bool beginFrame() override { return false; }
    void endFrame() override {}
    void setClearColor(const Vec4& /*rgba*/) override {}
    Extent2D framebufferExtent() const override { return {}; }
    void setPresentation(bool /*vsync*/, u32 /*sampleCount*/) override {}
    u32 presentationSampleCount() const override { return 1; }
    void setTextureFiltering(u32 /*filtering*/) override {}
    void setSmoothSprites(bool /*enabled*/) override {}
    void setAuthoredSilhouettes(bool /*enabled*/) override {}
    std::unique_ptr<Texture> createTexture(const TextureDesc& /*desc*/,
                                           std::span<const u8> /*rgba8Pixels*/) override {
        throw std::logic_error("Fixture inspection cannot create GPU resources");
    }
    void updateTexture(Texture& /*texture*/, std::span<const u8> /*rgba8Pixels*/) override {
        throw std::logic_error("Fixture inspection cannot update GPU resources");
    }
    const Texture& whiteTexture() const override { return *m_white; }
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state) override {
        // Decoders impose smaller limits; retain a local allocation bound for
        // inspection even if another optional provider supplies a malformed mesh.
        if (batch.vertexCount() <= usize{3} * 1024 * 1024) {
            m_draw(batch, texture, transform, state);
        }
    }
    void waitIdle() override {}
    bool applyDepthOfField(const DepthOfField& /*settings*/) override { return false; }
    bool applyBloom() override { return false; }
    void addHeatSource(const HeatSource& /*source*/) override {}
    bool applyAmbientOcclusion(const AmbientOcclusion& /*settings*/) override { return false; }

private:
    class White final : public Texture {
    public:
        u32 width() const override { return 1; }
        u32 height() const override { return 1; }
    } m_fallback;
    Draw m_draw;
    const Texture* m_white;
};
} // namespace

bool LegendsSceneAdapter::fixtureVisible(usize index) const {
    return index < m_fixtures.size() && !m_hiddenFixtures[index] && m_fixtureOpacity[index] > 0 &&
           m_fixtures[index].hasFigure();
}

void LegendsSceneAdapter::drawFixture(usize index, RenderDevice& device, const Mat4& clip,
                                      const CameraFrame& camera, TreeModel::Pass pass,
                                      f32 presentationAlpha) const {
    if (!fixtureVisible(index)) {
        return;
    }
    ItemFigure::Appearance appearance;
    appearance.whiteBlend = obeliskWhiteBlend(index);
    const auto locked = std::ranges::find_if(m_portalPresentation, [&](const auto& portal) {
        return !portal.enabled && portal.active == index;
    });
    if (locked != m_portalPresentation.end()) {
        appearance.hiddenMeshes = locked->lockedMeshes;
    }
    if (m_towerShardPortal == index) {
        appearance.hiddenMeshes = m_towerPortalHiddenMeshes;
    }
    // skodeath's display list clears G_CULL_BACK for every fire panel.
    // The circle must remain visible from inside as well as outside.
    appearance.doubleSided = m_towerRunePortal == index;
    const f32 opacity = m_towerShardPortal == index ? m_towerPortals[0].charge(presentationAlpha)
                                                    : m_fixtureOpacity[index];
    m_fixtures[index].draw(device, clip, WorldLighting{}, opacity, 1, &camera, pass,
                           presentationAlpha, &appearance);
}

usize LegendsSceneAdapter::fixtureIdentity(usize index) const {
    if (m_fixtureIdentities.size() != m_fixtures.size()) {
        m_fixtureIdentities.resize(m_fixtures.size());
        std::iota(m_fixtureIdentities.begin(), m_fixtureIdentities.end(), usize{0});
        const auto root = [&](usize value) {
            while (m_fixtureIdentities[value] != value) {
                value = m_fixtureIdentities[value];
            }
            return value;
        };
        const auto join = [&](usize first, usize second) {
            if (first >= m_fixtures.size() || second >= m_fixtures.size()) {
                return;
            }
            first = root(first);
            second = root(second);
            m_fixtureIdentities[std::max(first, second)] = std::min(first, second);
        };
        for (const auto& binding : m_containers) {
            join(binding.closedFigure, binding.openingFigure);
            join(binding.closedFigure, binding.brokenFigure);
            for (const auto& outcome : binding.outcomes) {
                join(binding.closedFigure, outcome.openingFigure);
            }
        }
        for (const auto& binding : m_gates) {
            join(binding.idleFigure, binding.openingFigure);
        }
        for (const auto& binding : m_obelisks) {
            join(binding.idleFigure, binding.spentFigure);
        }
        for (const auto& binding : m_switchPresentation) {
            if (binding.activeFigure) {
                join(binding.idleFigure, *binding.activeFigure);
            }
        }
        for (const auto& binding : m_generatorPresentation) {
            for (const auto variant : binding.figures) {
                if (variant) {
                    join(binding.originalFigure, *variant);
                }
            }
        }
        for (const auto& binding : m_traps) {
            std::optional<usize> first;
            for (const auto variant : binding.figures) {
                if (variant) {
                    first = first.value_or(*variant);
                    join(*first, *variant);
                }
            }
        }
        for (const auto& binding : m_arenaRocks) {
            std::optional<usize> first = binding.burstFigure;
            for (const auto variant : binding.figures) {
                if (variant) {
                    first = first.value_or(*variant);
                    join(*first, *variant);
                }
            }
        }
        for (const auto& binding : m_portalPresentation) {
            for (const auto variant : binding.figures) {
                join(binding.figures.front(), variant);
            }
        }
        for (const auto& binding : m_floorSelectors) {
            std::optional<usize> first;
            for (const auto& outcome : binding.outcomes) {
                if (outcome.figure) {
                    first = first.value_or(*outcome.figure);
                    join(*first, *outcome.figure);
                }
            }
        }
        for (const auto& binding : m_towerDoors) {
            join(binding.openingFigure, binding.openFigure);
        }
        if (m_tower) {
            for (const auto* variants : {&m_tower->sumner, &m_tower->podium}) {
                for (const auto& variant : *variants) {
                    join(variants->front().figure, variant.figure);
                }
            }
        }
        for (usize i = 0; i < m_fixtureIdentities.size(); ++i) {
            m_fixtureIdentities[i] = root(i);
        }
    }
    return m_fixtureIdentities[index];
}

std::optional<ImportedWorldObject> LegendsSceneAdapter::fixtureObject(s32 id) const {
    if (id < kFixtureObjectBase) {
        return {};
    }
    const auto index = static_cast<usize>(id - kFixtureObjectBase);
    if (index >= m_fixtures.size() || fixtureIdentity(index) != index) {
        return {};
    }
    ImportedWorldObject result;
    result.id = id;
    result.name = m_fixtures[index].tree()->name;
    result.position = m_fixtures[index].position();
    result.visible = false;
    for (usize i = 0; i < m_fixtures.size(); ++i) {
        if (fixtureIdentity(i) == index && fixtureVisible(i)) {
            result.visible = true;
            result.position = m_fixtures[i].position();
        }
    }
    return result;
}

std::optional<ImportedObjectHit> LegendsSceneAdapter::pickFixture(const Vec3& origin,
                                                                  const Vec3& direction,
                                                                  const CameraFrame& camera,
                                                                  f32 alpha) const {
    if (!finite(origin) || !finite(direction) || glm::length(direction) < 1e-7f ||
        !std::isfinite(alpha)) {
        return {};
    }
    const Vec3 ray = glm::normalize(direction);
    std::optional<f32> nearest;
    usize selected = 0;
    usize current = 0;
    FixtureInspection inspection{
        [&](const ImmediateBatch& batch, const Texture&, const Mat4& transform, const DrawState&) {
            const auto vertices = batch.triangles();
            for (usize i = 0; i + 2 < vertices.size(); i += 3) {
                if (vertices[i].color.a == 0 && vertices[i + 1].color.a == 0 &&
                    vertices[i + 2].color.a == 0) {
                    continue;
                }
                const auto at = [&](usize corner) {
                    return Vec3{transform * Vec4{vertices[i + corner].position, 1}};
                };
                if (const auto hit = triangleHit(origin, ray, at(0), at(1), at(2));
                    hit && (!nearest || *hit < *nearest)) {
                    nearest = hit;
                    selected = current;
                }
            }
        }};
    for (; current < m_fixtures.size(); ++current) {
        drawFixture(current, inspection, Mat4{1}, camera, TreeModel::Pass::All, alpha);
    }
    if (!nearest || selected >= static_cast<usize>(kFixtureObjectBase)) {
        return {};
    }
    const auto object =
        fixtureObject(kFixtureObjectBase + static_cast<s32>(fixtureIdentity(selected)));
    return object ? std::optional{ImportedObjectHit{*object, *nearest}} : std::nullopt;
}

void LegendsSceneAdapter::highlightFixture(RenderDevice& device, const Mat4& clip,
                                           const CameraFrame& camera, f32 alpha, s32 id,
                                           const SelectionHighlight& highlight) const {
    const auto selected = fixtureObject(id);
    if (!selected || !selected->visible || !std::isfinite(alpha)) {
        return;
    }
    const auto index = static_cast<usize>(id - kFixtureObjectBase);
    FixtureInspection inspection{
        [&](const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
            const DrawState& state) {
            Mesh mesh;
            MeshPart part;
            const auto vertices = batch.triangles();
            mesh.vertices.reserve(vertices.size());
            part.indices.reserve(vertices.size());
            for (usize i = 0; i + 2 < vertices.size(); i += 3) {
                const Vec3 a{transform * Vec4{vertices[i].position, 1}};
                const Vec3 b{transform * Vec4{vertices[i + 1].position, 1}};
                const Vec3 c{transform * Vec4{vertices[i + 2].position, 1}};
                if (!finite(a) || !finite(b) || !finite(c)) {
                    continue;
                }
                Vec3 normal = glm::cross(b - a, c - a);
                if (glm::length(normal) > 1e-7f) {
                    normal = glm::normalize(normal);
                }
                for (usize corner = 0; corner < 3; ++corner) {
                    const auto& vertex = vertices[i + corner];
                    part.indices.push_back(static_cast<u32>(mesh.vertices.size()));
                    mesh.vertices.push_back({.position = Vec3{transform * Vec4{vertex.position, 1}},
                                             .normal = normal,
                                             .uv = vertex.uv * state.uvScale + state.uvOffset});
                }
            }
            highlight.draw(device, clip, mesh, part, Mat4{1}, texture);
        },
        &device.whiteTexture()};
    for (usize i = 0; i < m_fixtures.size(); ++i) {
        if (fixtureIdentity(i) == index) {
            drawFixture(i, inspection, Mat4{1}, camera, TreeModel::Pass::All, alpha);
        }
    }
}
} // namespace gdl::game
