#include "engine/world/TreeModel.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <stdexcept>
#include <string_view>

#include "engine/core/Log.h"

namespace gdl {

namespace {

/** Marker nodes, kept unseen: a figure's, and the place in a chest where its contents lie. */
constexpr std::array<std::string_view, 2> kMarkerNodes{"DUMMY", "NULL1"};

} // namespace

TreeModel::Shape TreeModel::makeShape(const Mesh& mesh, TextureSet& textures,
                                      RenderDevice& device) {
    Shape shape;
    shape.mesh = &mesh;
    for (const MeshPart& part : mesh.parts) {
        if (part.texture >= textures.size()) {
            throw std::runtime_error("mesh refers to a texture outside the set");
        }
        shape.textures.push_back(&textures.texture(device, part.texture));
        shape.translucent.push_back(textures.entry(part.texture).translucent());
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
                     RenderDevice& device) {
    clear();
    bool first = true;
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        const TreeNodeInfo& info = tree.nodes[i];
        const bool flips = std::ranges::any_of(
            info.objectFrames, [](const auto& run) { return !run.object.empty(); });
        if ((info.object.empty() && !flips) || std::ranges::find(kMarkerNodes, info.name) != kMarkerNodes.end()) {
            continue;
        }
        try {
            Node node;
            node.index = i;
            node.offset = tree.worldPosition(i);
            node.chrome = info.chrome();
            node.additive = info.additive();
            node.depthWrite = info.writesDepth();
            node.facing = CameraFrame::facingOf(info.objectFlags);
            if (!info.object.empty()) {
                const auto model = models.find(info.object);
                if (!model.has_value()) {
                    throw std::runtime_error(std::format("object {} is missing", info.object));
                }
                node.shape = makeShape(models.mesh(*model), textures, device);
                include(node.shape, node.offset, first);
            }
            // An object node's runs follow the set's order from the run's first object.
            for (const TreeNodeInfo::ObjectFrames& run : info.objectFrames) {
                FrameRun frames;
                frames.start = run.start;
                if (!run.object.empty()) {
                    const auto model = models.find(run.object);
                    if (!model.has_value()) {
                        throw std::runtime_error(std::format("object {} is missing", run.object));
                    }
                    const u32 firstModel = *model;
                    for (s32 f = 0;
                         f < run.frames && firstModel + static_cast<u32>(f) < models.size(); ++f) {
                        frames.shapes.push_back(makeShape(
                            models.mesh(firstModel + static_cast<u32>(f)), textures, device));
                        include(frames.shapes.back(), node.offset, first);
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
        if (node.runs.empty()) {
            continue;
        }
        node.shape = Shape{};
        if (sequence >= node.runs.size()) {
            continue;
        }
        const FrameRun& run = node.runs[sequence];
        const auto count = static_cast<s32>(run.shapes.size());
        const s32 at = frame - run.start;
        if (at >= 0 && at < count) {
            node.shape = run.shapes[static_cast<usize>(at)];
        } else if (count == 1) {
            node.shape = run.shapes[0];
        }
    }
}

void TreeModel::draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
                     const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                     const CameraFrame* camera, f32 alpha) const {
    if (alpha <= 0.0f) {
        return;
    }
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, false);
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, true);
}

void TreeModel::drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                          const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                          const CameraFrame* camera, f32 alpha, bool translucent) const {
    const bool fading = alpha < 1.0f;
    for (const Node& node : m_nodes) {
        if (node.shape.mesh == nullptr) {
            continue;
        }
        Mat4 placement = node.index < nodeTransforms.size()
                             ? model * nodeTransforms[node.index]
                             : glm::translate(model, node.offset);
        if (camera != nullptr && node.facing != 0) {
            placement = camera->face(placement, node.facing);
        }
        const Mat3 normalMatrix{placement};
        const Shape& shape = node.shape;
        for (usize p = 0; p < shape.mesh->parts.size(); ++p) {
            // A fading figure blends its solid parts too, so the whole of it thins together.
            const bool blended = shape.translucent[p] || node.additive || fading;
            if (blended != translucent) {
                continue;
            }
            const MeshPart& part = shape.mesh->parts[p];
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            for (const u32 index : part.indices) {
                const MeshVertex& v = shape.mesh->vertices[index];
                const Vec3 normal = glm::normalize(normalMatrix * v.normal);
                const Vec2 uv =
                    node.chrome ? Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)} : v.uv;
                const Vec4 placed = placement * Vec4{v.position, 1.0f};
                // Glows add their whole texture; the original never lights them.
                Color color = node.additive || m_unlit ? Color::white() : lighting.shade(normal);
                color.r = static_cast<u8>(static_cast<u32>(color.r) * m_tint.r / 255);
                color.g = static_cast<u8>(static_cast<u32>(color.g) * m_tint.g / 255);
                color.b = static_cast<u8>(static_cast<u32>(color.b) * m_tint.b / 255);
                if (fading) {
                    color.a = static_cast<u8>(static_cast<f32>(color.a) * alpha);
                }
                m_batch.vertex(Vec3{placed}, color, uv);
            }
            m_batch.end();
            DrawState state;
            state.cullBack = true;
            state.blend = node.additive ? BlendMode::Additive : BlendMode::Alpha;
            if (m_maskedTexture != nullptr && !blended) {
                state.blend = BlendMode::Opaque;
            }
            state.maskedTexture = m_maskedTexture;
            state.alphaTest = blended ? DrawState::kTranslucentAlphaTest : 0.0f;
            state.depthWrite = node.depthWrite && m_depthWrite && !fading;
            state.uvOffset = textureOffset(shape.slots[p]);
            state.uvScale = textureScale(shape.slots[p]);
            const Texture* texture = shape.textures[p];
            for (const auto& [slot, frame] : m_frames) {
                if (slot == shape.slots[p]) {
                    texture = frame;
                }
            }
            device.draw(m_batch, *texture, clip, state);
        }
    }
}

void TreeModel::setTextureFrame(u32 slot, const Texture* frame) {
    std::erase_if(m_frames, [slot](const auto& shown) { return shown.first == slot; });
    if (frame != nullptr) {
        m_frames.emplace_back(slot, frame);
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
