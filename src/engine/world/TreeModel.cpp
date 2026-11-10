#include "engine/world/TreeModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/TextureBindings.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

/** Marker nodes, kept unseen: a figure's, and the place in a chest where its contents lie. */
constexpr std::array<std::string_view, 2> kMarkerNodes{"DUMMY", "NULL1"};

} // namespace

bool TreeModel::compatibleMorph(const Mesh& from, const Mesh& to) {
    if (from.parts.size() != to.parts.size() || from.prelit != to.prelit) {
        return false;
    }
    for (usize i = 0; i < from.parts.size(); ++i) {
        if (from.parts[i].rgbLightmap != to.parts[i].rgbLightmap ||
            (from.parts[i].rgbLightmap && from.parts[i].lightmap != to.parts[i].lightmap)) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<u32>> TreeModel::morphMap(const Mesh& from, const Mesh& to) {
    if (!compatibleMorph(from, to)) {
        return std::nullopt;
    }
    bool sameOrder = from.vertices.size() == to.vertices.size();
    for (usize p = 0; p < from.parts.size() && sameOrder; ++p) {
        sameOrder = from.parts[p].indices == to.parts[p].indices;
    }
    if (sameOrder) {
        return std::vector<u32>{};
    }
    // The frames were written with their vertices in another order, or with some split
    // or joined (the grunts' archer, the demons and the knights, among others). A vertex
    // keeps its texture coordinate and colour from frame to frame and moves a little: its
    // nearest such vertex in the next frame is it, shared when the next frame has fewer.
    // One with no such partner, or whose nearest stands further off than half the body,
    // is not the same vertex, and the pair is left to cut as the original does.
    Vec3 low{std::numeric_limits<f32>::max()};
    Vec3 high{std::numeric_limits<f32>::lowest()};
    for (const MeshVertex& v : from.vertices) {
        low = glm::min(low, v.position);
        high = glm::max(high, v.position);
    }
    const f32 farthest = 0.5f * glm::distance(low, high);
    const bool lightmapped = std::ranges::any_of(from.parts, &MeshPart::rgbLightmap);
    std::vector<u32> map(from.vertices.size(), 0);
    for (usize i = 0; i < from.vertices.size(); ++i) {
        const MeshVertex& v = from.vertices[i];
        f32 best = std::numeric_limits<f32>::max();
        bool found = false;
        for (usize j = 0; j < to.vertices.size(); ++j) {
            const MeshVertex& w = to.vertices[j];
            if (w.uv != v.uv || w.color != v.color ||
                (lightmapped && w.lightmapUv != v.lightmapUv)) {
                continue;
            }
            const f32 distance = glm::distance(v.position, w.position);
            if (distance < best) {
                best = distance;
                map[i] = static_cast<u32>(j);
                found = true;
            }
        }
        if (!found || best > farthest) {
            return std::nullopt;
        }
    }
    return map;
}

TreeModel::Shape TreeModel::makeShape(const Mesh& mesh, TextureSet& textures, RenderDevice& device,
                                      std::span<TextureSet* const> lenders) {
    Shape shape;
    shape.mesh = &mesh;
    const bool lightmapped = std::ranges::any_of(mesh.parts, &MeshPart::rgbLightmap);
    for (const MeshPart& part : mesh.parts) {
        if (part.texture >= textures.size()) {
            throw std::runtime_error("mesh refers to a texture outside the set");
        }
        const auto& entry = textures.entry(part.texture);
        const auto binding = TextureBindings(textures, lenders).slot(part.texture);
        if (!binding) {
            throw std::runtime_error("external texture was not lent: " + entry.name);
        }
        TextureSet* source = binding->set;
        const u32 index = binding->index;
        shape.textures.push_back(&source->texture(device, index));
        shape.translucent.push_back(entry.translucent() || source->entry(index).translucent());
        shape.slots.push_back(part.texture);
        const Texture* lightmap = nullptr;
        if (part.rgbLightmap) {
            if (part.lightmap == 0 || part.lightmap >= textures.size()) {
                throw std::runtime_error("mesh refers to a lightmap outside the set");
            }
            lightmap = &textures.texture(device, part.lightmap);
        }
        if (lightmapped) {
            shape.lightmaps.push_back(lightmap);
        }
    }
    cutBySilhouettes(shape, textures, device, lenders);
    return shape;
}

namespace {

/** The flat part a silhouette mask lies over, and every vertex's place in the mask. */
struct SilhouetteCut {
    usize part = 0;
    std::vector<Vec2> uv;
};

std::optional<SilhouetteCut> silhouetteCut(const Mesh& mesh, usize maskPart,
                                           const Image& silhouette) {
    const MeshPart& mask = mesh.parts[maskPart];
    if (mask.indices.size() < 3) {
        return std::nullopt;
    }
    f32 lowest = std::numeric_limits<f32>::max();
    f32 highest = std::numeric_limits<f32>::lowest();
    Vec2 lo{std::numeric_limits<f32>::max()};
    Vec2 hi{std::numeric_limits<f32>::lowest()};
    for (const u32 index : mask.indices) {
        const Vec3& at = mesh.vertices[index].position;
        lowest = std::min(lowest, at.y);
        highest = std::max(highest, at.y);
        lo = glm::min(lo, Vec2{at.x, at.z});
        hi = glm::max(hi, Vec2{at.x, at.z});
    }
    constexpr f32 kFlat = 0.05f;  // a mask lies level
    constexpr f32 kUnder = 0.25f; // and what it cuts lies this close beneath or above it
    if (highest - lowest > kFlat) {
        return std::nullopt;
    }
    // The mask's first triangle maps the ground onto its picture.
    const MeshVertex& a = mesh.vertices[mask.indices[0]];
    const MeshVertex& b = mesh.vertices[mask.indices[1]];
    const MeshVertex& c = mesh.vertices[mask.indices[2]];
    const glm::mat2 ground{Vec2{b.position.x - a.position.x, b.position.z - a.position.z},
                           Vec2{c.position.x - a.position.x, c.position.z - a.position.z}};
    if (std::abs(glm::determinant(ground)) < 1e-6f) {
        return std::nullopt;
    }
    const glm::mat2 toTriangle = glm::inverse(ground);
    const auto picture = [&](const Vec3& at) {
        const Vec2 st = toTriangle * Vec2{at.x - a.position.x, at.z - a.position.z};
        return a.uv + st.x * (b.uv - a.uv) + st.y * (c.uv - a.uv);
    };
    // What stands off the ground (a generator's mound over its pool) keeps whole: it is
    // placed at the set texel nearest the shape's centre.
    u64 sumX = 0;
    u64 sumY = 0;
    u64 set = 0;
    for (u32 y = 0; y < silhouette.height; ++y) {
        for (u32 x = 0; x < silhouette.width; ++x) {
            if (silhouette.pixel(x, y).a != 0) {
                sumX += x;
                sumY += y;
                ++set;
            }
        }
    }
    if (set == 0) {
        return std::nullopt;
    }
    const auto centreX = static_cast<u32>(sumX / set);
    const auto centreY = static_cast<u32>(sumY / set);
    Vec2 keep{0.5f, 0.5f};
    u64 nearest = std::numeric_limits<u64>::max();
    for (u32 y = 0; y < silhouette.height; ++y) {
        for (u32 x = 0; x < silhouette.width; ++x) {
            if (silhouette.pixel(x, y).a == 0) {
                continue;
            }
            const u64 dx = x > centreX ? x - centreX : centreX - x;
            const u64 dy = y > centreY ? y - centreY : centreY - y;
            if (dx * dx + dy * dy < nearest) {
                nearest = dx * dx + dy * dy;
                keep = {(static_cast<f32>(x) + 0.5f) / static_cast<f32>(silhouette.width),
                        (static_cast<f32>(y) + 0.5f) / static_cast<f32>(silhouette.height)};
            }
        }
    }
    const Vec2 margin = (hi - lo) * 0.05f;
    const auto under = [&](const Vec3& at) {
        return std::abs(at.y - lowest) <= kUnder && at.x >= lo.x - margin.x &&
               at.x <= hi.x + margin.x && at.z >= lo.y - margin.y && at.z <= hi.y + margin.y;
    };
    // A triangle is cut only when it lies level under the mask: the pool, not the mound that
    // rises from it.
    const auto level = [&](const MeshPart& part, usize triangle) {
        for (usize c = 0; c < 3; ++c) {
            if (!under(mesh.vertices[part.indices[triangle + c]].position)) {
                return false;
            }
        }
        return true;
    };
    usize best = maskPart;
    usize bestCount = 0;
    for (usize q = 0; q < mesh.parts.size(); ++q) {
        if (q == maskPart) {
            continue;
        }
        usize count = 0;
        for (usize k = 0; k + 2 < mesh.parts[q].indices.size(); k += 3) {
            count += level(mesh.parts[q], k) ? 1 : 0;
        }
        if (count > bestCount) {
            best = q;
            bestCount = count;
        }
    }
    if (bestCount == 0) {
        return std::nullopt;
    }
    // A vertex a sloped triangle shares keeps whole, so nothing rising is ever cut.
    constexpr u8 kLevel = 1;
    constexpr u8 kSloped = 2;
    std::vector<u8> roles(mesh.vertices.size(), 0);
    const MeshPart& part = mesh.parts[best];
    for (usize k = 0; k + 2 < part.indices.size(); k += 3) {
        const u8 role = level(part, k) ? kLevel : kSloped;
        for (usize c = 0; c < 3; ++c) {
            roles[part.indices[k + c]] |= role;
        }
    }
    SilhouetteCut cut;
    cut.part = best;
    cut.uv.assign(mesh.vertices.size(), keep);
    for (usize index = 0; index < mesh.vertices.size(); ++index) {
        if (roles[index] == kLevel) {
            cut.uv[index] = picture(mesh.vertices[index].position);
        }
    }
    return cut;
}

} // namespace

void TreeModel::cutBySilhouettes(Shape& shape, TextureSet& textures, RenderDevice& device,
                                 std::span<TextureSet* const> lenders) {
    shape.cutouts.assign(shape.mesh->parts.size(), nullptr);
    shape.hidden.assign(shape.mesh->parts.size(), false);
    for (usize p = 0; p < shape.mesh->parts.size(); ++p) {
        const auto binding = TextureBindings(textures, lenders).slot(shape.mesh->parts[p].texture);
        if (!binding) {
            continue;
        }
        const Image* silhouette = binding->set->silhouette(binding->index);
        if (silhouette == nullptr) {
            continue;
        }
        const auto cut = silhouetteCut(*shape.mesh, p, *silhouette);
        if (!cut) {
            continue;
        }
        const Texture* texture = binding->set->silhouetteTexture(device, binding->index);
        if (texture == nullptr) {
            continue;
        }
        shape.hidden[p] = true;
        shape.cutouts[cut->part] = texture;
        shape.cutoutUv = cut->uv;
    }
}

void TreeModel::include(const Shape& shape, const Vec3& offset, bool& first) {
    for (const MeshVertex& v : shape.mesh->vertices) {
        const Vec3 position = v.position + offset;
        m_min = first ? position : glm::min(m_min, position);
        m_max = first ? position : glm::max(m_max, position);
        first = false;
    }
}

bool TreeModel::bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures,
                     RenderDevice& device, std::span<TextureSet* const> lenders) {
    clear();
    bool first = true;
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        const TreeNodeInfo& info = tree.nodes[i];
        const bool flips = std::ranges::any_of(info.objectFrames,
                                               [](const auto& run) { return !run.object.empty(); });
        if ((info.object.empty() && !flips) ||
            std::ranges::find(kMarkerNodes, info.name) != kMarkerNodes.end()) {
            continue;
        }
        try {
            Node node;
            node.index = i;
            for (s32 parent = static_cast<s32>(i);
                 parent >= 0 && static_cast<usize>(parent) < tree.nodes.size() &&
                 node.ancestors.size() < tree.nodes.size();
                 parent = tree.nodes[static_cast<usize>(parent)].parent) {
                node.ancestors.push_back(static_cast<usize>(parent));
            }
            node.offset = tree.worldPosition(i);
            const auto material = ObjectMaterial::fromFlags(info.objectFlags);
            node.chrome = material.chrome;
            node.additive = material.additive;
            node.depthWrite = material.depthWrite;
            node.depthTest = material.depthTest;
            node.sorted = material.sorted;
            node.deferAlways = material.deferAlways;
            node.behind = material.sortBias < 0.0f;
            node.facing = material.facing;
            if (!info.object.empty()) {
                const auto model = models.find(info.object);
                if (!model.has_value()) {
                    throw std::runtime_error(std::format("object {} is missing", info.object));
                }
                node.shape = makeShape(models.mesh(*model), textures, device, lenders);
                include(node.shape, node.offset, first);
            }
            // An object node's runs follow the set's order from the run's first object.
            for (const TreeNodeInfo::ObjectFrames& run : info.objectFrames) {
                FrameRun frames;
                frames.start = run.start;
                if (node.runs.size() < tree.sequences.size()) {
                    const auto& sequence = tree.sequences[node.runs.size()];
                    if ((sequence.flags & 1U) != 0) {
                        frames.reverseLength = sequence.frames;
                    }
                }
                if (!run.object.empty()) {
                    const auto model = models.find(run.object);
                    if (!model.has_value()) {
                        throw std::runtime_error(std::format("object {} is missing", run.object));
                    }
                    const u32 firstModel = *model;
                    for (s32 f = 0;
                         f < run.frames && firstModel + static_cast<u32>(f) < models.size(); ++f) {
                        frames.shapes.push_back(
                            makeShape(models.mesh(firstModel + static_cast<u32>(f)), textures,
                                      device, lenders));
                        include(frames.shapes.back(), node.offset, first);
                    }
                    for (usize f = 1; f < frames.shapes.size(); ++f) {
                        const auto& from = frames.shapes[f - 1];
                        const auto& to = frames.shapes[f];
                        std::optional<std::vector<u32>> ahead;
                        std::optional<std::vector<u32>> back;
                        // A frame's own textures stay on it while its vertices move on (the
                        // vermin change texture with the frame); only translucency must agree.
                        if (run.interpolate && from.translucent == to.translucent) {
                            ahead = morphMap(*from.mesh, *to.mesh);
                            back = ahead ? morphMap(*to.mesh, *from.mesh) : std::nullopt;
                        }
                        frames.morphs.push_back(ahead.has_value() && back.has_value());
                        frames.forward.push_back(ahead.value_or(std::vector<u32>{}));
                        frames.backward.push_back(back.value_or(std::vector<u32>{}));
                    }
                }
                node.runs.push_back(std::move(frames));
            }
            m_nodes.push_back(std::move(node));
        } catch (const std::exception& e) {
            log::warn("Tree model {}: {}", tree.name, e.what());
            clear();
            return false;
        }
    }
    return !m_nodes.empty();
}

namespace {

/** The colour between two, by `blend` of the way from the first. */
Color mixColor(const Color& from, const Color& to, f32 blend) {
    const auto channel = [blend](u8 a, u8 b) {
        return static_cast<u8>(std::clamp(
            std::lround(static_cast<f32>(a) + (static_cast<f32>(b) - static_cast<f32>(a)) * blend),
            0L, 255L));
    };
    return Color{channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b),
                 channel(from.a, to.a)};
}

} // namespace

void TreeModel::setFrame(u32 sequence, s32 frame) {
    for (Node& node : m_nodes) {
        selectFrame(node, sequence, frame);
    }
}

void TreeModel::setSubtreeFrame(usize root, u32 sequence, s32 frame) {
    for (Node& node : m_nodes) {
        if (std::ranges::find(node.ancestors, root) != node.ancestors.end()) {
            selectFrame(node, sequence, frame);
        }
    }
}

void TreeModel::setPresentationFrame(u32 sequence, f32 frame) {
    for (Node& node : m_nodes) {
        selectPresentationFrame(node, sequence, frame);
    }
}
void TreeModel::setMeshPresentationFrame(usize index, u32 sequence, f32 frame) {
    for (Node& node : m_nodes) {
        if (node.index == index) {
            selectPresentationFrame(node, sequence, frame);
        }
    }
}
void TreeModel::selectPresentationFrame(Node& node, u32 sequence, f32 frame) {
    const auto whole = static_cast<s32>(std::floor(std::max(frame, 0.0f)));
    const f32 fraction = std::clamp(frame - static_cast<f32>(whole), 0.0f, 1.0f);
    selectFrame(node, sequence, whole);
    if (sequence >= node.runs.size() || fraction <= 0.0f) {
        return;
    }
    const auto& run = node.runs[sequence];
    const s32 at = (run.reverseLength > 0 ? run.reverseLength - whole - 1 : whole) - run.start;
    const s32 next = at + (run.reverseLength > 0 ? -1 : 1);
    if (at < 0 || next < 0 || at >= static_cast<s32>(run.shapes.size()) ||
        next >= static_cast<s32>(run.shapes.size()) ||
        !run.morphs[static_cast<usize>(std::min(at, next))]) {
        return;
    }
    node.nextMesh = run.shapes[static_cast<usize>(next)].mesh;
    const auto pair = static_cast<usize>(std::min(at, next));
    const std::vector<u32>& map = next > at ? run.forward[pair] : run.backward[pair];
    node.nextMap = map.empty() ? nullptr : &map;
    node.meshBlend = fraction;
}

void TreeModel::selectFrame(Node& node, u32 sequence, s32 frame) {
    node.nextMesh = nullptr;
    node.nextMap = nullptr;
    node.meshBlend = 0.0f;
    if (node.runs.empty()) {
        return;
    }
    node.shape = Shape{};
    if (sequence >= node.runs.size()) {
        return;
    }
    const FrameRun& run = node.runs[sequence];
    const auto count = static_cast<s32>(run.shapes.size());
    const s32 sampled = run.reverseLength > 0 ? run.reverseLength - frame - 1 : frame;
    const s32 at = sampled - run.start;
    if (at >= 0 && at < count) {
        node.shape = run.shapes[static_cast<usize>(at)];
    } else if (count == 1) {
        node.shape = run.shapes[0];
    }
}

void TreeModel::draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
                     const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                     const CameraFrame* camera, f32 alpha, Pass pass, Occlusion occlusion) const {
    if (alpha <= 0.0f) {
        return;
    }
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, false, pass, occlusion);
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, true, pass, occlusion);
}

void TreeModel::drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                          const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                          const CameraFrame* camera, f32 alpha, bool translucent, Pass pass,
                          Occlusion occlusion) const {
    for (const Node& node : m_nodes) {
        const f32 opacity = alpha * node.alpha;
        const bool dissolveSkin = m_maskedTexture != nullptr && m_skinAlpha == SkinAlpha::Dissolve;
        const bool nodeFlash =
            node.maskedTexture != nullptr && (m_maskedTexture == nullptr || dissolveSkin);
        const Texture* mask = nodeFlash ? node.maskedTexture : m_maskedTexture;
        const bool dissolving = dissolveSkin && !nodeFlash;
        const bool fading = opacity < 1.0f;
        // Retail -4 alternate skins preserve the authored Z-write flag. This
        // renderer splits solid/effects passes: defer dissolves like opacity
        // fades so their partial alpha cannot mask solids drawn later.
        const bool depthWrite = node.depthWrite && m_appearance.depthWrite &&
                                (!fading || m_fadedDepthWrite) && !dissolving;
        if ((pass == Pass::DepthWriting && !depthWrite) || (pass == Pass::Effects && depthWrite)) {
            continue;
        }
        if (node.shape.mesh == nullptr || opacity <= 0.0f) {
            continue;
        }
        Mat4 placement = node.index < nodeTransforms.size() ? model * nodeTransforms[node.index]
                                                            : glm::translate(model, node.offset);
        if (camera != nullptr && node.facing != 0) {
            placement = camera->face(placement, node.facing);
        }
        const Mat3 normalMatrix{placement};
        const Shape& shape = node.shape;
        // The authored rule decides what waits: a sortable node that is faded, glowing,
        // flagged behind or wears an alpha-textured mesh. A glow or a fading or dissolving
        // figure waits as well, so it composites over what it stands among.
        const bool additive = node.additive || m_appearance.additive;
        const bool deferredNode =
            node.sorted && (opacity < 1.0f || node.deferAlways || shape.mesh->alphaTextured());
        // The original draws its items in placement order; this renderer draws the pickups
        // before the fixtures they may stand against, so a pickup's alpha-textured mesh
        // (the authored object bit) composites after them as a whole.
        const bool blendedPass =
            pass == Pass::Blended || pass == Pass::BlendedAhead || pass == Pass::BlendedBehind;
        const bool pickupPass = pass == Pass::Opaque || blendedPass;
        const bool blended = deferredNode || additive || fading || dissolving ||
                             (pickupPass && shape.mesh->alphaTextured());
        if (blended != translucent) {
            continue;
        }
        for (usize p = 0; p < shape.mesh->parts.size(); ++p) {
            const bool deferred = blended || !depthWrite;
            // Of the deferred parts, the ones sorted behind wait for the scenery's own
            // deferred queue when the caller splits the blended pass.
            const bool waits = deferred && node.behind;
            if ((pass == Pass::Opaque && deferred) || (blendedPass && !deferred) ||
                (pass == Pass::BlendedAhead && waits) || (pass == Pass::BlendedBehind && !waits) ||
                (p < shape.hidden.size() && shape.hidden[p])) {
                continue;
            }
            const MeshPart& part = shape.mesh->parts[p];
            // A silhouette cut shares the second stage with a skin mask; the mask wins.
            const Texture* cutout =
                mask == nullptr && p < shape.cutouts.size() ? shape.cutouts[p] : nullptr;
            const Texture* lightmap = mask == nullptr && cutout == nullptr && !additive &&
                                              !m_appearance.unlit && p < shape.lightmaps.size()
                                          ? shape.lightmaps[p]
                                          : nullptr;
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            for (const u32 index : part.indices) {
                const MeshVertex& v = shape.mesh->vertices[index];
                Vec3 position = v.position;
                Vec3 vertexNormal = v.normal;
                Vec2 meshUv = v.uv;
                Vec2 lightmapUv = v.lightmapUv;
                Color baked = v.color;
                if (node.nextMesh != nullptr) {
                    const MeshVertex& next =
                        node.nextMesh
                            ->vertices[node.nextMap != nullptr ? (*node.nextMap)[index] : index];
                    position = glm::mix(position, next.position, node.meshBlend);
                    const Vec3 blendedNormal = glm::mix(vertexNormal, next.normal, node.meshBlend);
                    if (glm::dot(blendedNormal, blendedNormal) > 0.000001f) {
                        vertexNormal = blendedNormal;
                    }
                    meshUv = glm::mix(meshUv, next.uv, node.meshBlend);
                    lightmapUv = glm::mix(lightmapUv, next.lightmapUv, node.meshBlend);
                    baked = mixColor(baked, next.color, node.meshBlend);
                }
                const Vec3 normal = glm::normalize(normalMatrix * vertexNormal);
                const Vec2 uv =
                    node.chrome ? Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)} : meshUv;
                const Vec4 placed = placement * Vec4{position, 1.0f};
                // Glows add their whole texture; the original never lights them.
                // Baked RGB belongs to the geometry stream, including level meshes
                // rendered as destructible items rather than by WorldScene.
                Color color = shape.mesh->prelit ? baked : lighting.shade(Vec3{placed}, normal);
                if (additive || m_appearance.unlit || nodeFlash || lightmap != nullptr) {
                    color = Color::white();
                }
                const Color& tint = m_appearance.tint;
                color.r = static_cast<u8>(static_cast<u32>(color.r) * tint.r / 255);
                color.g = static_cast<u8>(static_cast<u32>(color.g) * tint.g / 255);
                color.b = static_cast<u8>(static_cast<u32>(color.b) * tint.b / 255);
                if (fading) {
                    color.a = static_cast<u8>(static_cast<f32>(color.a) * opacity);
                }
                if (cutout != nullptr) {
                    m_batch.vertex(Vec3{placed}, color, uv, shape.cutoutUv[index]);
                } else if (lightmap != nullptr) {
                    m_batch.vertex(Vec3{placed}, color, uv,
                                   lightmapUv / Vec2{lightmap->width(), lightmap->height()});
                } else {
                    m_batch.vertex(Vec3{placed}, color, uv);
                }
            }
            m_batch.end();
            DrawState state;
            state.whiteBlend = m_whiteBlend;
            state.mipmaps = true;
            const auto sampling =
                std::ranges::find(m_textureSampling, shape.slots[p], &TextureSampling::slot);
            if (sampling != m_textureSampling.end()) {
                state.clampTexture = sampling->clamp;
                state.mipmaps = sampling->mipmaps;
            }
            // This pass explicitly requests colour compositing, not coverage. Native
            // pickup shadows and gas can write depth while still requiring smooth alpha.
            // Native death skins (-4 in MBTreeSetAltTex) use their own alpha.
            // Coverage replacement is only for opaque alternate skins/hit flashes.
            state.alphaToCoverage = !blendedPass && !dissolving;
            state.cullBack = m_cullBack;
            state.blend = additive ? BlendMode::Additive : BlendMode::Alpha;
            // A baked colour is darkened as drawn; a lit part has it in the light it was
            // given, with the lanterns' light over it.
            state.darken =
                additive || (!shape.mesh->prelit && lightmap == nullptr) ? 0.0f : lighting.darken;
            if (mask != nullptr && !blended) {
                state.blend = BlendMode::Opaque;
            }
            state.maskedTexture = mask;
            if (cutout != nullptr) {
                state.lightmap = cutout;
                state.lightmapCutout = true;
            } else if (lightmap != nullptr) {
                state.lightmap = lightmap;
                state.lightmapRgb = true;
            }
            // This explicit pass belongs to world pickups, not actor/world cutouts or UI.
            state.smoothSprite = blendedPass && node.facing != 0 && blended;
            // setPrimAlpha sets the console's alpha compare (greater than 2) for every
            // object; a solid cutout's clear texels never show or write depth.
            state.alphaTest = DrawState::kTranslucentAlphaTest;
            state.depthWrite = depthWrite;
            state.depthTest = node.depthTest || occlusion == Occlusion::SolidWorld;
            state.uvOffset = textureOffset(shape.slots[p]);
            state.uvScale = textureScale(shape.slots[p]);
            if (node.uvOffset.has_value()) {
                state.uvOffset = *node.uvOffset;
                state.uvScale = node.uvScale;
            }
            const Texture* texture = shape.textures[p];
            const TextureFrame* selected = nullptr;
            for (const auto& frame : m_frames) {
                if (frame.slot == shape.slots[p]) {
                    selected = &frame;
                }
            }
            for (const auto& frame : node.frames) {
                if (frame.slot == shape.slots[p]) {
                    selected = &frame;
                }
            }
            if (selected != nullptr) {
                texture = selected->frame;
                // Dissolve and white-flash skins keep their coverage. Solid cutouts
                // remain discrete: blending their silhouettes would change occlusion.
                if (mask == nullptr && (additive || !node.depthWrite)) {
                    state.nextTexture = selected->next;
                    state.textureBlend = selected->blend;
                }
            }
            device.draw(m_batch, *texture, clip, state);
            if (m_highlight != nullptr) {
                m_highlight->draw(device, clip, *shape.mesh, part, placement, *texture);
            }
        }
    }
}

void TreeModel::setTextureFrame(u32 slot, const Texture* frame, const Texture* next, f32 blend) {
    std::erase_if(m_frames, [slot](const auto& shown) { return shown.slot == slot; });
    if (frame != nullptr) {
        m_frames.push_back({slot, frame, next, blend});
    }
}

void TreeModel::setTextureSampling(u32 slot, bool clamp, bool mipmaps) {
    std::erase_if(m_textureSampling, [slot](const auto& entry) { return entry.slot == slot; });
    m_textureSampling.push_back({slot, clamp, mipmaps});
}

void TreeModel::setTextureOffset(u32 slot, const Vec2& offset, const Vec2& scale) {
    for (Slide& slide : m_offsets) {
        if (slide.slot == slot) {
            slide.offset = offset;
            slide.scale = scale;
            return;
        }
    }
    m_offsets.push_back(Slide{slot, offset, scale});
}

void TreeModel::resetTextures() {
    m_maskedTexture = nullptr;
    m_skinAlpha = SkinAlpha::PreserveCoverage;
    m_frames.clear();
    m_offsets.clear();
    for (Node& node : m_nodes) {
        node.frames.clear();
        node.uvOffset.reset();
        node.uvScale = Vec2{1.0f};
        node.alpha = 1.0f;
        node.maskedTexture = nullptr;
    }
}

void TreeModel::setNodeAlpha(usize root, f32 alpha) {
    for (Node& node : m_nodes) {
        if (std::ranges::find(node.ancestors, root) != node.ancestors.end()) {
            node.alpha = std::clamp(alpha, 0.0f, 1.0f);
        }
    }
}

void TreeModel::setMeshAlpha(usize node, f32 alpha) {
    for (Node& part : m_nodes) {
        if (part.index == node) {
            part.alpha = std::clamp(alpha, 0.0f, 1.0f);
        }
    }
}

void TreeModel::setMeshMaskedTexture(usize index, const Texture* texture) {
    for (Node& node : m_nodes) {
        if (node.index == index) {
            node.maskedTexture = texture;
        }
    }
}

void TreeModel::setNodeMaskedTexture(usize root, const Texture* texture) {
    for (Node& node : m_nodes) {
        if (std::ranges::find(node.ancestors, root) != node.ancestors.end()) {
            node.maskedTexture = texture;
        }
    }
}

void TreeModel::setNodeTextureFrame(usize root, u32 slot, const Texture* frame, const Texture* next,
                                    f32 blend) {
    for (Node& node : m_nodes) {
        if (std::ranges::find(node.ancestors, root) == node.ancestors.end()) {
            continue;
        }
        std::erase_if(node.frames, [slot](const auto& shown) { return shown.slot == slot; });
        if (frame != nullptr) {
            node.frames.push_back({slot, frame, next, blend});
        }
    }
}

void TreeModel::setNodeTextureOffset(usize root, const Vec2& offset, const Vec2& scale) {
    for (Node& node : m_nodes) {
        if (std::ranges::find(node.ancestors, root) != node.ancestors.end()) {
            node.uvOffset = offset;
            node.uvScale = scale;
        }
    }
}

Vec2 TreeModel::textureOffset(u32 slot) const {
    for (const Slide& slide : m_offsets) {
        if (slide.slot == slot) {
            return slide.offset;
        }
    }
    return Vec2{0.0f, 0.0f};
}

Vec2 TreeModel::textureScale(u32 slot) const {
    for (const Slide& slide : m_offsets) {
        if (slide.slot == slot) {
            return slide.scale;
        }
    }
    return Vec2{1.0f, 1.0f};
}

} // namespace gdl
