#include "engine/world/TreeModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
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
    if (from.vertices.size() != to.vertices.size() || from.parts.size() != to.parts.size() ||
        from.prelit != to.prelit) {
        return false;
    }
    for (usize p = 0; p < from.parts.size(); ++p) {
        if (from.parts[p].indices != to.parts[p].indices ||
            from.parts[p].texture != to.parts[p].texture ||
            from.parts[p].lightmap != to.parts[p].lightmap) {
            return false;
        }
    }
    for (usize v = 0; v < from.vertices.size(); ++v) {
        if (from.vertices[v].uv != to.vertices[v].uv ||
            from.vertices[v].lightmapUv != to.vertices[v].lightmapUv ||
            from.vertices[v].color != to.vertices[v].color) {
            return false;
        }
    }
    return true;
}

TreeModel::Shape TreeModel::makeShape(const Mesh& mesh, TextureSet& textures, RenderDevice& device,
                                      std::span<TextureSet* const> lenders) {
    Shape shape;
    shape.mesh = &mesh;
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
    }
    return shape;
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
                        frames.morphs.push_back(
                            from.textures == to.textures && from.translucent == to.translucent &&
                            from.slots == to.slots && compatibleMorph(*from.mesh, *to.mesh));
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
    const auto whole = static_cast<s32>(std::floor(std::max(frame, 0.0f)));
    const f32 fraction = std::clamp(frame - static_cast<f32>(whole), 0.0f, 1.0f);
    setFrame(sequence, whole);
    for (Node& node : m_nodes) {
        if (sequence >= node.runs.size() || fraction <= 0.0f) {
            continue;
        }
        const auto& run = node.runs[sequence];
        const s32 at = (run.reverseLength > 0 ? run.reverseLength - whole - 1 : whole) - run.start;
        const s32 next = at + (run.reverseLength > 0 ? -1 : 1);
        if (at < 0 || next < 0 || at >= static_cast<s32>(run.shapes.size()) ||
            next >= static_cast<s32>(run.shapes.size()) ||
            !run.morphs[static_cast<usize>(std::min(at, next))]) {
            continue;
        }
        node.nextMesh = run.shapes[static_cast<usize>(next)].mesh;
        node.meshBlend = fraction;
    }
}

void TreeModel::selectFrame(Node& node, u32 sequence, s32 frame) {
    node.nextMesh = nullptr;
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
        const bool fading = opacity < 1.0f;
        const bool depthWrite = node.depthWrite && m_depthWrite && !fading;
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
        for (usize p = 0; p < shape.mesh->parts.size(); ++p) {
            // A fading figure blends its solid parts too, so the whole of it thins together.
            const bool additive = node.additive || m_additive;
            const bool blended = shape.translucent[p] || additive || fading;
            if (blended != translucent) {
                continue;
            }
            const MeshPart& part = shape.mesh->parts[p];
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            for (const u32 index : part.indices) {
                const MeshVertex& v = shape.mesh->vertices[index];
                Vec3 position = v.position;
                Vec3 vertexNormal = v.normal;
                if (node.nextMesh != nullptr) {
                    const MeshVertex& next = node.nextMesh->vertices[index];
                    position = glm::mix(position, next.position, node.meshBlend);
                    const Vec3 blendedNormal = glm::mix(vertexNormal, next.normal, node.meshBlend);
                    if (glm::dot(blendedNormal, blendedNormal) > 0.000001f) {
                        vertexNormal = blendedNormal;
                    }
                }
                const Vec3 normal = glm::normalize(normalMatrix * vertexNormal);
                const Vec2 uv =
                    node.chrome ? Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)} : v.uv;
                const Vec4 placed = placement * Vec4{position, 1.0f};
                // Glows add their whole texture; the original never lights them.
                const bool flash = node.maskedTexture != nullptr && m_maskedTexture == nullptr;
                // Baked RGB belongs to the geometry stream, including level meshes
                // rendered as destructible items rather than by WorldScene.
                Color color = shape.mesh->prelit ? v.color : lighting.shade(Vec3{placed}, normal);
                if (additive || m_unlit || flash) {
                    color = Color::white();
                }
                color.r = static_cast<u8>(static_cast<u32>(color.r) * m_tint.r / 255);
                color.g = static_cast<u8>(static_cast<u32>(color.g) * m_tint.g / 255);
                color.b = static_cast<u8>(static_cast<u32>(color.b) * m_tint.b / 255);
                if (fading) {
                    color.a = static_cast<u8>(static_cast<f32>(color.a) * opacity);
                }
                m_batch.vertex(Vec3{placed}, color, uv);
            }
            m_batch.end();
            DrawState state;
            state.cullBack = m_cullBack;
            state.blend = additive ? BlendMode::Additive : BlendMode::Alpha;
            const Texture* mask = m_maskedTexture != nullptr ? m_maskedTexture : node.maskedTexture;
            if (mask != nullptr && !blended) {
                state.blend = BlendMode::Opaque;
            }
            state.maskedTexture = mask;
            state.alphaTest = blended ? DrawState::kTranslucentAlphaTest : 0.0f;
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
        }
    }
}

void TreeModel::setTextureFrame(u32 slot, const Texture* frame, const Texture* next, f32 blend) {
    std::erase_if(m_frames, [slot](const auto& shown) { return shown.slot == slot; });
    if (frame != nullptr) {
        m_frames.push_back({slot, frame, next, blend});
    }
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
