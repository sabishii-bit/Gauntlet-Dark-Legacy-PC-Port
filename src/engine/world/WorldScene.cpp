#include "engine/world/WorldScene.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ranges>

#include "engine/core/Log.h"

namespace gdl {

namespace {

constexpr std::uint32_t kLightmapShift = 20U;
constexpr std::uint64_t kAdditiveKey = std::uint64_t{1} << 40U;
constexpr std::uint64_t kNoDepthKey = std::uint64_t{1} << 41U;

/** The coordinates a chromed surface samples: its normal's x and y folded into the map. */
Vec2 chromeUv(const Vec3& normal) {
    return Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)};
}

} // namespace

WorldScene::Slot& WorldScene::slotFor(std::uint32_t index, TextureSet& textures,
                                      RenderDevice& device, std::span<TextureSet* const> lenders) {
    if (const auto found = m_slots.find(index); found != m_slots.end()) {
        return found->second;
    }
    Slot& slot = m_slots[index];
    if (index >= textures.size()) {
        log::warn("World scene: texture {} is not in the set", index);
        return slot;
    }
    try {
        const TextureSetEntry& entry = textures.entry(index);
        if (!entry.external()) {
            slot.texture = &textures.texture(device, index);
            slot.translucent = entry.translucent();
            slot.usable = true;
            return slot;
        }
        // An external texture is another archive's, found by name.
        for (TextureSet* lender : lenders) {
            if (lender == nullptr) {
                continue;
            }
            if (const auto lent = lender->find(entry.name); lent.has_value()) {
                slot.texture = &lender->texture(device, *lent);
                slot.translucent = lender->entry(*lent).translucent() || entry.translucent();
                slot.usable = true;
                return slot;
            }
        }
        log::warn("World scene: external texture {} ({}) was not lent; drawn white", index,
                  entry.name);
        slot.texture = &device.whiteTexture();
        slot.translucent = entry.translucent();
        slot.usable = true;
    } catch (const std::exception& e) {
        log::warn("World scene: texture {} skipped: {}", index, e.what());
        slot.usable = false;
    }
    return slot;
}

bool WorldScene::build(const WorldLayout& layout, ModelSet& models, TextureSet& textures,
                       RenderDevice& device, const WorldLighting& lighting,
                       std::span<TextureSet* const> lenders) {
    clear();
    m_lighting = lighting;
    const std::vector<WorldObject>& objects = layout.objects();
    m_placements.resize(objects.size());
    std::vector<std::uint8_t> animated(objects.size(), 0);
    for (const WorldAnimation& animation : layout.animations()) {
        if (animation.object >= 0 && static_cast<std::size_t>(animation.object) < objects.size()) {
            animated[static_cast<std::size_t>(animation.object)] = 1;
        }
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        Placement& placement = m_placements[i];
        placement.local = glm::translate(Mat4{1.0f}, objects[i].position);
        placement.parent = objects[i].parent;
        for (std::int32_t at = static_cast<std::int32_t>(i); at >= 0;
             at = objects[static_cast<std::size_t>(at)].parent) {
            if (animated[static_cast<std::size_t>(at)] != 0) {
                placement.moving = true;
                break;
            }
        }
    }

    // Still geometry is keyed by texture and lightmap, the glows and the objects that keep
    // depth unwritten apart from the rest.
    std::unordered_map<std::uint64_t, std::size_t> batchByKey;
    const auto batchFor = [&](std::uint32_t slot, std::uint32_t lightmap, bool additive,
                              bool depthWrite) -> Batch& {
        const std::uint64_t key = std::uint64_t{slot} |
                                  (std::uint64_t{lightmap} << kLightmapShift) |
                                  (additive ? kAdditiveKey : 0) | (depthWrite ? 0 : kNoDepthKey);
        if (const auto found = batchByKey.find(key); found != batchByKey.end()) {
            return m_batches[found->second];
        }
        Batch batch;
        batch.slot = slot;
        if (lightmap != 0 && lightmap < textures.size()) {
            const TextureSetEntry& map = textures.entry(lightmap);
            batch.lightmap = &textures.texture(device, lightmap);
            batch.lightmapScale = Vec2{1.0f / static_cast<float>(std::max(map.width, 1U)),
                                       1.0f / static_cast<float>(std::max(map.height, 1U))};
        }
        batch.translucent = m_slots[slot].translucent;
        batch.additive = additive;
        batch.depthWrite = depthWrite;
        batch.geometry.begin(PrimitiveTopology::TriangleList);
        m_batches.push_back(std::move(batch));
        batchByKey[key] = m_batches.size() - 1;
        return m_batches.back();
    };

    for (std::size_t i = 0; i < objects.size(); ++i) {
        const WorldObject& object = objects[i];
        // A particle system's marker is where its effect plays, not something to draw.
        if (object.particles()) {
            continue;
        }
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
        const bool chrome = (object.objectFlags & WorldObject::kChrome) != 0;
        const bool additive = object.additive();
        const bool prelit = object.prelit() && mesh->prelit;
        const bool depthWrite = (object.objectFlags & WorldObject::kNoDepthWrite) == 0;
        const std::uint32_t facing = CameraFrame::facingOf(object.objectFlags);
        const bool unit = m_placements[i].moving || object.sorted() || facing != 0;
        Unit placedUnit;
        placedUnit.object = i;
        placedUnit.mesh = mesh;
        placedUnit.chrome = chrome;
        placedUnit.sorted = object.sorted();
        placedUnit.depthWrite = depthWrite;
        placedUnit.facing = facing;
        placedUnit.prelit = prelit;
        if ((object.objectFlags & WorldObject::kSortBehind) != 0) {
            placedUnit.sortBias = kSortBehindBias;
        } else if ((object.objectFlags & WorldObject::kSortBack) != 0) {
            placedUnit.sortBias = kSortBackBias;
        }
        const Vec3 offset = layout.worldPosition(i);
        bool placed = false;
        for (const MeshPart& part : mesh->parts) {
            const Slot& slot = slotFor(part.texture, textures, device, lenders);
            if (!slot.usable) {
                continue;
            }
            try {
                if (unit) {
                    UnitPart unitPart;
                    unitPart.slot = part.texture;
                    unitPart.part = &part;
                    if (part.lightmap != 0 && part.lightmap < textures.size()) {
                        const TextureSetEntry& map = textures.entry(part.lightmap);
                        unitPart.lightmap = &textures.texture(device, part.lightmap);
                        unitPart.lightmapScale =
                            Vec2{1.0f / static_cast<float>(std::max(map.width, 1U)),
                                 1.0f / static_cast<float>(std::max(map.height, 1U))};
                    }
                    unitPart.translucent = slot.translucent;
                    unitPart.additive = additive;
                    placedUnit.parts.push_back(unitPart);
                } else {
                    Batch& batch = batchFor(part.texture, part.lightmap, additive, depthWrite);
                    for (const std::uint32_t index : part.indices) {
                        const MeshVertex& v = mesh->vertices[index];
                        batch.geometry.vertex(
                            v.position + offset, shadeOf(additive, prelit, v, v.normal, lighting),
                            chrome ? chromeUv(v.normal) : v.uv, v.lightmapUv * batch.lightmapScale);
                    }
                }
            } catch (const std::exception& e) {
                log::warn("World scene: {}: {}", object.name, e.what());
                continue;
            }
            m_triangles += part.indices.size() / 3;
            placed = true;
        }
        if (placed) {
            ++m_placed;
            if (unit) {
                m_units.push_back(std::move(placedUnit));
            }
        }
    }
    for (Batch& batch : m_batches) {
        batch.geometry.end();
    }
    std::erase_if(m_batches, [](const Batch& batch) { return batch.geometry.empty(); });
    // Opaque first so translucent surfaces blend over what stands behind them, and the glows
    // last so they add onto everything.
    const auto pass = [](const Batch& batch) {
        if (batch.additive) {
            return 2;
        }
        return batch.translucent ? 1 : 0;
    };
    std::stable_sort(m_batches.begin(), m_batches.end(),
                     [&](const Batch& a, const Batch& b) { return pass(a) < pass(b); });
    m_world.assign(objects.size(), Mat4{1.0f});
    m_worldValid.assign(objects.size(), 0);
    if (!built()) {
        log::warn("World scene: no placed object has a mesh");
        return false;
    }
    return true;
}

void WorldScene::clear() {
    m_slots.clear();
    m_batches.clear();
    m_units.clear();
    m_placements.clear();
    m_world.clear();
    m_worldValid.clear();
    m_order.clear();
    m_placed = 0;
    m_triangles = 0;
}

bool WorldScene::moving(std::size_t object) const {
    return object < m_placements.size() && m_placements[object].moving;
}

void WorldScene::setObjectTransform(std::size_t object, const Mat4& local) {
    if (moving(object)) {
        m_placements[object].local = local;
        std::fill(m_worldValid.begin(), m_worldValid.end(), std::uint8_t{0});
    }
}

const Mat4& WorldScene::worldTransform(std::size_t object) const {
    static const Mat4 kIdentity{1.0f};
    if (object >= m_placements.size()) {
        return kIdentity;
    }
    return worldOf(object);
}

WorldScene::Unit* WorldScene::unitOf(std::size_t object) {
    for (Unit& unit : m_units) {
        if (unit.object == object) {
            return &unit;
        }
    }
    return nullptr;
}

const WorldScene::Unit* WorldScene::unitOf(std::size_t object) const {
    for (const Unit& unit : m_units) {
        if (unit.object == object) {
            return &unit;
        }
    }
    return nullptr;
}

void WorldScene::setObjectAlpha(std::size_t object, float alpha) {
    if (Unit* unit = unitOf(object); unit != nullptr) {
        unit->alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

float WorldScene::objectAlpha(std::size_t object) const {
    const Unit* unit = unitOf(object);
    return unit != nullptr ? unit->alpha : 1.0f;
}

void WorldScene::setTextureFrame(std::uint32_t slot, const Texture* texture) {
    if (const auto found = m_slots.find(slot); found != m_slots.end()) {
        found->second.frame = texture;
    }
}

void WorldScene::setTextureOffset(std::uint32_t slot, const Vec2& offset) {
    if (const auto found = m_slots.find(slot); found != m_slots.end()) {
        found->second.offset = offset;
    }
}

const Texture* WorldScene::textureOf(std::uint32_t slot) const {
    const auto found = m_slots.find(slot);
    return found != m_slots.end() && found->second.usable ? found->second.current() : nullptr;
}

Vec2 WorldScene::textureOffset(std::uint32_t slot) const {
    const auto found = m_slots.find(slot);
    return found != m_slots.end() ? found->second.offset : Vec2{0.0f, 0.0f};
}

/** An object's placement composed with every ancestor's, for the frame being drawn: the
 * chain up to the nearest ancestor already composed, then composed back down. */
const Mat4& WorldScene::worldOf(std::size_t object) const {
    m_chain.clear();
    for (std::int32_t at = static_cast<std::int32_t>(object);
         at >= 0 && m_worldValid[static_cast<std::size_t>(at)] == 0;
         at = m_placements[static_cast<std::size_t>(at)].parent) {
        m_chain.push_back(static_cast<std::size_t>(at));
    }
    for (const std::size_t index : m_chain | std::views::reverse) {
        const Placement& placement = m_placements[index];
        m_world[index] = placement.parent >= 0
                             ? m_world[static_cast<std::size_t>(placement.parent)] * placement.local
                             : placement.local;
        m_worldValid[index] = 1;
    }
    return m_world[object];
}

void WorldScene::drawBatch(RenderDevice& device, const Batch& batch, const Mat4& clip) const {
    const Slot& slot = m_slots.at(batch.slot);
    DrawState state;
    state.blend = batch.additive ? BlendMode::Additive : BlendMode::Alpha;
    state.lightmap = batch.lightmap;
    state.uvOffset = slot.offset;
    state.alphaTest = batch.translucent ? kAlphaTest : 0.0f;
    state.cullBack = true;
    state.depthWrite = batch.depthWrite;
    state.darken = batch.additive ? 0.0f : m_darken;
    device.draw(batch.geometry, *slot.current(), clip, state);
}

Color WorldScene::shadeOf(bool additive, bool prelit, const MeshVertex& vertex, const Vec3& normal,
                          const WorldLighting& lighting) {
    if (additive) {
        return kUnlit;
    }
    return prelit ? vertex.color : lighting.shade(normal);
}

/** Places, lights and draws a unit's parts: its opaque ones when `opaque`, its translucent
 * and glowing ones when `translucent`. */
void WorldScene::drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                          const CameraFrame& camera, bool opaque, bool translucent) const {
    if (unit.alpha <= 0.0f) {
        return;
    }
    const Mat4 world = camera.face(worldOf(unit.object), unit.facing);
    const Mat3 normalMatrix{world};
    for (const UnitPart& part : unit.parts) {
        // A fading unit's solid parts blend too, so the whole of it thins together.
        const bool blended = part.translucent || part.additive || unit.alpha < 1.0f;
        if (blended ? !translucent : !opaque) {
            continue;
        }
        const Slot& slot = m_slots.at(part.slot);
        m_scratch.clear();
        m_scratch.begin(PrimitiveTopology::TriangleList);
        for (const std::uint32_t index : part.part->indices) {
            const MeshVertex& v = unit.mesh->vertices[index];
            const Vec3 normal = glm::normalize(normalMatrix * v.normal);
            const Vec4 placed = world * Vec4{v.position, 1.0f};
            Color color = shadeOf(part.additive, unit.prelit, v, normal, m_lighting);
            if (unit.alpha < 1.0f) {
                color.a = static_cast<std::uint8_t>(static_cast<float>(color.a) * unit.alpha);
            }
            m_scratch.vertex(Vec3{placed}, color, unit.chrome ? chromeUv(normal) : v.uv,
                             v.lightmapUv * part.lightmapScale);
        }
        m_scratch.end();
        DrawState state;
        state.blend = part.additive ? BlendMode::Additive : BlendMode::Alpha;
        state.lightmap = part.lightmap;
        state.uvOffset = slot.offset;
        state.alphaTest = part.translucent ? kAlphaTest : 0.0f;
        state.cullBack = true;
        state.depthWrite = unit.depthWrite && unit.alpha >= 1.0f;
        state.darken = part.additive ? 0.0f : m_darken;
        device.draw(m_scratch, *slot.current(), clip, state);
    }
}

void WorldScene::draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera) const {
    const Vec3& eye = camera.position;
    std::fill(m_worldValid.begin(), m_worldValid.end(), std::uint8_t{0});
    std::size_t next = 0;
    while (next < m_batches.size() && !m_batches[next].translucent && !m_batches[next].additive) {
        drawBatch(device, m_batches[next++], clip);
    }
    // Moving objects' solid parts join the opaque; everything blended sorts by depth.
    for (const Unit& unit : m_units) {
        if (!unit.sorted) {
            drawUnit(device, unit, clip, camera, true, false);
        }
    }
    while (next < m_batches.size() && !m_batches[next].additive) {
        drawBatch(device, m_batches[next++], clip);
    }
    // Farthest first, like the original's sorted objects: their key is the depth away from
    // the eye, and the biases push the flagged ones behind everything else.
    m_order.resize(m_units.size());
    std::vector<float> keys(m_units.size());
    for (std::size_t i = 0; i < m_units.size(); ++i) {
        m_order[i] = i;
        const Vec3 origin{worldOf(m_units[i].object)[3]};
        keys[i] = -glm::distance(eye, origin) + m_units[i].sortBias;
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
    for (const std::size_t i : m_order) {
        const Unit& unit = m_units[i];
        drawUnit(device, unit, clip, camera, unit.sorted, true);
    }
    while (next < m_batches.size()) {
        drawBatch(device, m_batches[next++], clip);
    }
}

} // namespace gdl
