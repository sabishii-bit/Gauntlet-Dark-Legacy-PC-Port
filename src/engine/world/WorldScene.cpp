#include "engine/world/WorldScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>

#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/TextureBindings.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/ThermalMaterial.h"

namespace gdl {

namespace {

constexpr u32 kLightmapShift = 20U;
constexpr u64 kAdditiveKey = u64{1} << 40U;
constexpr u64 kNoDepthKey = u64{1} << 41U;
constexpr u64 kNoDepthTestKey = u64{1} << 42U;

/** Local TRS interpolation preserves a child's arc about its moving parent. */
Mat4 blendPlacement(const Mat4& previous, const Mat4& current, f32 alpha) {
    if (alpha >= 1.0f || previous == current) {
        return current;
    }
    Mat3 from{previous};
    Mat3 to{current};
    Vec3 fromScale{0};
    Vec3 toScale{0};
    constexpr f32 kMinimumScale = 1e-6f;
    constexpr f32 kBasisTolerance = 1e-3f;
    for (s32 axis = 0; axis < 3; ++axis) {
        fromScale[axis] = glm::length(from[axis]);
        toScale[axis] = glm::length(to[axis]);
        if (fromScale[axis] < kMinimumScale || toScale[axis] < kMinimumScale) {
            return current;
        }
        from[axis] /= fromScale[axis];
        to[axis] /= toScale[axis];
    }
    const auto orthogonal = [](const Mat3& basis) {
        return std::abs(glm::determinant(basis) - 1.0f) < kBasisTolerance &&
               std::abs(glm::dot(basis[0], basis[1])) < kBasisTolerance &&
               std::abs(glm::dot(basis[0], basis[2])) < kBasisTolerance &&
               std::abs(glm::dot(basis[1], basis[2])) < kBasisTolerance;
    };
    // Shear/reflection and large authored orientation cuts must not become an
    // unrelated rotation. Position may still move continuously beneath that basis.
    Mat4 result = current;
    if (orthogonal(from) && orthogonal(to)) {
        const Quat first = glm::quat_cast(from);
        const Quat second = glm::quat_cast(to);
        if (std::abs(glm::dot(first, second)) > std::cos(kHalfPi * 0.5f)) {
            result = glm::mat4_cast(glm::slerp(first, second, alpha));
            result = glm::scale(result, glm::mix(fromScale, toScale, alpha));
        }
    }
    result[3] = glm::mix(previous[3], current[3], alpha);
    return result;
}

/** The coordinates a chromed surface samples: its normal's x and y folded into the map. */
Vec2 chromeUv(const Vec3& normal) {
    return Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)};
}

/** One placed flame, not the union of every fire sharing its animated texture. */
void submitHeat(RenderDevice& device, const ImmediateBatch& geometry, const Mat4& clip,
                const CameraFrame& camera, f32 seconds) {
    if (geometry.empty()) {
        return;
    }
    Vec3 lowest = geometry.triangles().front().position;
    Vec3 highest = lowest;
    bool visible = false;
    for (const ImmediateVertex& vertex : geometry.triangles()) {
        lowest = glm::min(lowest, vertex.position);
        highest = glm::max(highest, vertex.position);
        visible = visible || vertex.color.a != 0;
    }
    constexpr f32 kMaximumRadius = 8.0f;
    const f32 radius = std::min(glm::length(highest - lowest) * 0.5f, kMaximumRadius);
    if (!visible || radius <= 0.0f) {
        return;
    }
    const Vec3 center = (lowest + highest) * 0.5f + Vec3{0, radius * 0.5f, 0};
    device.addHeatSource(
        HeatSource::project(clip, center, camera.right, camera.up, radius, seconds));
}

} // namespace

WorldScene::Slot& WorldScene::slotFor(u32 index, TextureSet& textures, RenderDevice& device,
                                      std::span<TextureSet* const> lenders) {
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
        if (const auto binding = TextureBindings(textures, lenders).slot(index)) {
            slot.texture = &binding->set->texture(device, binding->index);
            slot.translucent =
                binding->set->entry(binding->index).translucent() || entry.translucent();
            slot.thermal = ThermalMaterial::surface(entry.name);
            slot.usable = true;
            return slot;
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
                       std::span<TextureSet* const> lenders,
                       std::span<const usize> controlledObjects,
                       std::span<const usize> backgroundObjects) {
    clear();
    m_lighting = lighting;
    const std::vector<WorldObject>& objects = layout.objects();
    m_placements.resize(objects.size());
    std::vector<u8> animated(objects.size(), 0);
    for (const WorldAnimation& animation : layout.animations()) {
        if (animation.object >= 0 && static_cast<usize>(animation.object) < objects.size()) {
            animated[static_cast<usize>(animation.object)] = 1;
        }
    }
    for (usize i = 0; i < objects.size(); ++i) {
        Placement& placement = m_placements[i];
        placement.local = glm::translate(Mat4{1.0f}, objects[i].position);
        placement.initial = placement.local;
        placement.previous = placement.local;
        placement.parent = objects[i].parent;
        for (auto at = static_cast<s32>(i); at >= 0; at = objects[static_cast<usize>(at)].parent) {
            if (animated[static_cast<usize>(at)] != 0 ||
                (objects[static_cast<usize>(at)].flags & WorldObject::kAnimated) != 0) {
                placement.moving = true;
                break;
            }
        }
    }

    // Still geometry is keyed by texture and lightmap, the glows and the objects that keep
    // depth unwritten apart from the rest.
    std::unordered_map<u64, usize> batchByKey;
    const auto batchFor = [&](u32 slot, u32 lightmap, bool additive, bool depthWrite,
                              bool depthTest) -> Batch& {
        const u64 key = u64{slot} | (u64{lightmap} << kLightmapShift) |
                        (additive ? kAdditiveKey : 0) | (depthWrite ? 0 : kNoDepthKey) |
                        (depthTest ? 0 : kNoDepthTestKey);
        if (const auto found = batchByKey.find(key); found != batchByKey.end()) {
            return m_batches[found->second];
        }
        Batch batch;
        batch.slot = slot;
        if (lightmap != 0 && lightmap < textures.size()) {
            const TextureSetEntry& map = textures.entry(lightmap);
            batch.lightmap = &textures.texture(device, lightmap);
            batch.lightmapScale = Vec2{1.0f / static_cast<f32>(std::max(map.width, 1U)),
                                       1.0f / static_cast<f32>(std::max(map.height, 1U))};
        }
        batch.translucent = m_slots[slot].translucent;
        batch.additive = additive;
        batch.depthWrite = depthWrite;
        batch.depthTest = depthTest;
        batch.geometry.begin(PrimitiveTopology::TriangleList);
        m_batches.push_back(std::move(batch));
        batchByKey[key] = m_batches.size() - 1;
        return m_batches.back();
    };

    for (usize i = 0; i < objects.size(); ++i) {
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
        const auto material = ObjectMaterial::fromFlags(object.objectFlags);
        const bool chrome = material.chrome;
        const bool additive = material.additive;
        const bool prelit = object.prelit() && mesh->prelit;
        const bool depthWrite = material.depthWrite;
        const bool depthTest = material.depthTest;
        const u32 facing = material.facing;
        const bool background = std::ranges::find(backgroundObjects, i) != backgroundObjects.end();
        const bool thermal = std::ranges::any_of(mesh->parts, [&](const MeshPart& part) {
            return slotFor(part.texture, textures, device, lenders).thermal;
        });
        const bool unit = thermal || background || m_placements[i].moving || material.sorted ||
                          facing != 0 ||
                          std::ranges::find(controlledObjects, i) != controlledObjects.end();
        Unit placedUnit;
        placedUnit.object = i;
        placedUnit.mesh = mesh;
        placedUnit.chrome = chrome;
        placedUnit.sorted = material.sorted;
        placedUnit.background = background;
        placedUnit.depthWrite = depthWrite;
        placedUnit.depthTest = depthTest;
        placedUnit.facing = facing;
        placedUnit.prelit = prelit;
        placedUnit.sortBias = material.sortBias;
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
                            Vec2{1.0f / static_cast<f32>(std::max(map.width, 1U)),
                                 1.0f / static_cast<f32>(std::max(map.height, 1U))};
                    }
                    unitPart.translucent = slot.translucent;
                    unitPart.additive = additive;
                    placedUnit.parts.push_back(unitPart);
                } else {
                    Batch& batch =
                        batchFor(part.texture, part.lightmap, additive, depthWrite, depthTest);
                    for (const u32 index : part.indices) {
                        const MeshVertex& v = mesh->vertices[index];
                        const Vec3 at = v.position + offset;
                        batch.geometry.vertex(at, shadeOf(additive, prelit, v, v.normal, lighting),
                                              chrome ? chromeUv(v.normal) : v.uv,
                                              v.lightmapUv * batch.lightmapScale);
                        if (batch.normals.empty()) {
                            batch.lowest = at;
                            batch.highest = at;
                        }
                        batch.normals.push_back(v.normal);
                        batch.lowest = glm::min(batch.lowest, at);
                        batch.highest = glm::max(batch.highest, at);
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
                m_placements[i].unit = static_cast<s32>(m_units.size());
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
    m_presentedWorld.assign(objects.size(), Mat4{1.0f});
    m_presentedValid.assign(objects.size(), 0);
    // Stable across machines/pointer values. The build/assets handshake must still
    // authenticate the resource set; this catches a wrong level/order at restore.
    m_layoutSignature = 14695981039346656037ULL;
    const auto hashWord = [&](u32 word) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            m_layoutSignature =
                (m_layoutSignature ^ static_cast<u8>(word >> shift)) * 1099511628211ULL;
        }
    };
    hashWord(static_cast<u32>(objects.size()));
    for (usize i = 0; i < objects.size(); ++i) {
        hashWord(static_cast<u32>(objects[i].name.size()));
        for (const char value : objects[i].name) {
            hashWord(static_cast<u8>(value));
        }
        hashWord(static_cast<u32>(objects[i].parent));
        hashWord(objects[i].objectFlags);
        hashWord(objects[i].flags);
        for (s32 axis = 0; axis < 3; ++axis) {
            hashWord(std::bit_cast<u32>(objects[i].position[axis]));
        }
        hashWord(m_placements[i].moving ? 1U : 0U);
        hashWord(static_cast<u32>(m_placements[i].unit));
    }
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
    m_presentedWorld.clear();
    m_presentedValid.clear();
    m_order.clear();
    m_placed = 0;
    m_triangles = 0;
    m_textureFrame = 0;
    m_layoutSignature = 0;
}

bool WorldScene::moving(usize object) const {
    return object < m_placements.size() && m_placements[object].moving;
}

void WorldScene::capturePresentation() {
    for (Placement& placement : m_placements) {
        placement.previous = placement.local;
    }
}

void WorldScene::setObjectTransform(usize object, const Mat4& local, bool presentationCut) {
    if (moving(object)) {
        auto& placement = m_placements[object];
        if (presentationCut && placement.local != local && placement.continuity != 0) {
            placement.continuity = placement.continuity == std::numeric_limits<u32>::max()
                                       ? 0
                                       : placement.continuity + 1;
        }
        m_placements[object].local = local;
        if (presentationCut) {
            m_placements[object].previous = local;
        }
        std::fill(m_worldValid.begin(), m_worldValid.end(), u8{0});
    }
}

const Mat4& WorldScene::worldTransform(usize object) const {
    static const Mat4 kIdentity{1.0f};
    if (object >= m_placements.size()) {
        return kIdentity;
    }
    return worldOf(object);
}

WorldScene::Unit* WorldScene::unitOf(usize object) {
    const s32 index = object < m_placements.size() ? m_placements[object].unit : -1;
    return index >= 0 ? &m_units[static_cast<usize>(index)] : nullptr;
}

const WorldScene::Unit* WorldScene::unitOf(usize object) const {
    const s32 index = object < m_placements.size() ? m_placements[object].unit : -1;
    return index >= 0 ? &m_units[static_cast<usize>(index)] : nullptr;
}

SceneGeometry WorldScene::geometry() const {
    SceneGeometry result;
    result.layout = m_layoutSignature;
    result.objectCount = static_cast<u32>(m_placements.size());
    result.darken = m_darken;
    for (usize i = 0; i < m_placements.size(); ++i) {
        const auto& placement = m_placements[i];
        const auto* unit = unitOf(i);
        if (placement.moving || (unit != nullptr && (unit->alpha != 1 || !unit->visible))) {
            result.objects.push_back({static_cast<u32>(i), placement.continuity, placement.local,
                                      unit != nullptr ? unit->alpha : 1,
                                      unit == nullptr || unit->visible});
        }
    }
    return result;
}
bool WorldScene::acceptsGeometry(const SceneGeometry& state) const {
    if (!state.valid() || state.layout != m_layoutSignature ||
        state.objectCount != m_placements.size()) {
        return false;
    }
    usize cursor = 0;
    for (usize i = 0; i < m_placements.size(); ++i) {
        const auto& placement = m_placements[i];
        const auto* object = cursor < state.objects.size() && state.objects[cursor].index == i
                                 ? &state.objects[cursor++]
                                 : nullptr;
        if (object == nullptr) {
            if (placement.moving) {
                return false; // a missing moving parent must not silently use local geometry
            }
            continue;
        }
        const auto* unit = unitOf(i);
        if ((!placement.moving &&
             (unit == nullptr || object->local != placement.initial || object->continuity != 1)) ||
            (unit == nullptr && (!object->visible || object->alpha != 1))) {
            return false;
        }
    }
    return true;
}
bool WorldScene::applyGeometry(const SceneGeometry& state) {
    if (!acceptsGeometry(state)) {
        return false;
    }
    for (auto& unit : m_units) {
        unit.visible = true;
        unit.alpha = 1;
    }
    for (const auto& object : state.objects) {
        auto& placement = m_placements[object.index];
        placement.local = object.local;
        placement.previous = object.local;
        placement.continuity = object.continuity;
        if (auto* unit = unitOf(object.index)) {
            unit->alpha = object.alpha;
            unit->visible = object.visible;
        }
    }
    m_darken = state.darken;
    std::fill(m_worldValid.begin(), m_worldValid.end(), u8{0});
    std::fill(m_presentedValid.begin(), m_presentedValid.end(), u8{0});
    return true;
}

void WorldScene::setObjectAlpha(usize object, f32 alpha) {
    if (Unit* unit = unitOf(object); unit != nullptr) {
        unit->alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

f32 WorldScene::objectAlpha(usize object) const {
    const Unit* unit = unitOf(object);
    return unit != nullptr ? unit->alpha : 1.0f;
}

bool WorldScene::setObjectVisible(usize object, bool visible) {
    if (Unit* unit = unitOf(object); unit != nullptr) {
        unit->visible = visible;
        return true;
    }
    return false;
}

bool WorldScene::objectVisible(usize object) const {
    const Unit* unit = unitOf(object);
    return object < m_placements.size() && (unit == nullptr || unit->visible);
}

void WorldScene::setTextureFrame(u32 slot, const Texture* texture) {
    if (const auto found = m_slots.find(slot); found != m_slots.end()) {
        found->second.frame = texture;
        found->second.cycle.clear();
    }
}

void WorldScene::setTextureCycle(u32 slot, std::span<const Texture* const> frames, f32 position,
                                 f32 speed) {
    if (const auto found = m_slots.find(slot); found != m_slots.end()) {
        found->second.cycle.assign(frames.begin(), frames.end());
        found->second.cyclePosition = position;
        found->second.cycleSpeed = speed;
    }
}

const Texture* WorldScene::Slot::presentedTexture(DrawState& state,
                                                  std::optional<f32> frameOffset) const {
    if (!frameOffset || cycle.empty() || state.lightmap != nullptr ||
        (state.depthWrite && state.blend != BlendMode::Additive)) {
        return current();
    }
    const f32 sample = cyclePosition + *frameOffset * cycleSpeed;
    const auto count = static_cast<f32>(cycle.size());
    const f32 wrapped = sample - std::floor(sample / count) * count;
    const auto index = static_cast<usize>(wrapped);
    state.nextTexture = cycle[(index + 1) % cycle.size()];
    state.textureBlend = wrapped - std::floor(wrapped);
    return cycle[index];
}

void WorldScene::setTextureOffset(u32 slot, const Vec2& offset) {
    setTextureScroll(slot, offset, Vec2{0}, Vec2{0});
}

void WorldScene::setTextureScroll(u32 slot, const Vec2& offset, const Vec2& phase,
                                  const Vec2& velocity) {
    if (const auto found = m_slots.find(slot); found != m_slots.end()) {
        found->second.offset = offset;
        found->second.scrollPhase = phase;
        found->second.scrollVelocity = velocity;
    }
}

Vec2 WorldScene::Slot::presentedOffset(std::optional<f32> frameOffset) const {
    if (!frameOffset) {
        return offset;
    }
    const Vec2 sampled = offset + scrollPhase + scrollVelocity * *frameOffset;
    const auto wrap = [](f32 value, f32 velocity) {
        const f32 sign = velocity < 0 ? -1.0f : 1.0f;
        return velocity != 0 ? sign * glm::fract(sign * value) : value;
    };
    return {wrap(sampled.x, scrollVelocity.x), wrap(sampled.y, scrollVelocity.y)};
}

const Texture* WorldScene::textureOf(u32 slot) const {
    const auto found = m_slots.find(slot);
    return found != m_slots.end() && found->second.usable ? found->second.current() : nullptr;
}

Vec2 WorldScene::textureOffset(u32 slot) const {
    const auto found = m_slots.find(slot);
    return found != m_slots.end() ? found->second.offset : Vec2{0.0f, 0.0f};
}

/** An object's placement composed with every ancestor's, for the frame being drawn: the
 * chain up to the nearest ancestor already composed, then composed back down. */
const Mat4& WorldScene::worldOf(usize object) const {
    m_chain.clear();
    for (auto at = static_cast<s32>(object); at >= 0 && m_worldValid[static_cast<usize>(at)] == 0;
         at = m_placements[static_cast<usize>(at)].parent) {
        m_chain.push_back(static_cast<usize>(at));
    }
    for (const usize index : m_chain | std::views::reverse) {
        const Placement& placement = m_placements[index];
        m_world[index] = placement.parent >= 0
                             ? m_world[static_cast<usize>(placement.parent)] * placement.local
                             : placement.local;
        m_worldValid[index] = 1;
    }
    return m_world[object];
}

const Mat4& WorldScene::presentedWorldOf(usize object, f32 alpha) const {
    if (alpha < 0.0f) {
        return worldOf(object);
    }
    m_chain.clear();
    for (auto at = static_cast<s32>(object);
         at >= 0 && m_presentedValid[static_cast<usize>(at)] == 0;
         at = m_placements[static_cast<usize>(at)].parent) {
        m_chain.push_back(static_cast<usize>(at));
    }
    for (const usize index : m_chain | std::views::reverse) {
        const Placement& placement = m_placements[index];
        const Mat4 local = placement.moving ? blendPlacement(placement.previous, placement.local,
                                                             std::clamp(alpha, 0.0f, 1.0f))
                                            : placement.local;
        m_presentedWorld[index] =
            placement.parent >= 0 ? m_presentedWorld[static_cast<usize>(placement.parent)] * local
                                  : local;
        m_presentedValid[index] = 1;
    }
    return m_presentedWorld[object];
}

void WorldScene::drawBatch(RenderDevice& device, const Batch& batch, const Mat4& clip,
                           std::optional<f32> textureFrameOffset) const {
    const Slot& slot = m_slots.at(batch.slot);
    DrawState state;
    state.mipmaps = true;
    state.alphaToCoverage = true;
    state.blend = batch.additive ? BlendMode::Additive : BlendMode::Alpha;
    state.lightmap = batch.lightmap;
    state.uvOffset = slot.presentedOffset(textureFrameOffset);
    // pbSetDORegs/setPrimAlpha enable GX_GREATER with reference 2 independently of the
    // blend flag. Binary-alpha scenery (S8 wheat) belongs to the solid pass, but its clear
    // texels must not write depth and hide subsequently drawn scenery.
    state.alphaTest = kAlphaTest;
    state.cullBack = true;
    state.depthWrite = batch.depthWrite;
    state.depthTest = batch.depthTest;
    state.darken = batch.additive ? 0.0f : m_darken;
    // Where a point light reaches it, a copy with the lights added is drawn instead.
    const Vec3 centre = (batch.lowest + batch.highest) * 0.5f;
    if (!batch.additive && !m_lighting.points.empty() &&
        m_lighting.pointsReach(centre, glm::distance(centre, batch.highest))) {
        const std::span<const ImmediateVertex> vertices = batch.geometry.triangles();
        m_lit.clear();
        m_lit.begin(PrimitiveTopology::TriangleList);
        for (usize i = 0; i < vertices.size(); ++i) {
            ImmediateVertex v = vertices[i];
            if (m_lighting.pointsReach(v.position, 0.0f)) {
                v.color = m_lighting.brighten(v.color, v.position, batch.normals[i]);
            }
            m_lit.vertex(v);
        }
        m_lit.end();
        const Texture* texture = slot.presentedTexture(state, textureFrameOffset);
        device.draw(m_lit, *texture, clip, state);
        return;
    }
    const Texture* texture = slot.presentedTexture(state, textureFrameOffset);
    device.draw(batch.geometry, *texture, clip, state);
}

void WorldScene::setPointLights(std::span<const PointLight> points) {
    m_lighting.points.assign(points.begin(), points.end());
}

Color WorldScene::shadeOf(bool additive, bool prelit, const MeshVertex& vertex, const Vec3& normal,
                          const WorldLighting& lighting) {
    if (additive) {
        return kUnlit;
    }
    return prelit ? vertex.color : lighting.shade(normal);
}

Color WorldScene::shadeAt(bool additive, bool prelit, const MeshVertex& vertex,
                          const Vec3& position, const Vec3& normal, const WorldLighting& lighting) {
    if (additive) {
        return kUnlit;
    }
    return prelit ? lighting.brighten(vertex.color, position, normal)
                  : lighting.shade(position, normal);
}

/** Places, lights and draws a unit's parts: its opaque ones when `opaque`, its translucent
 * and glowing ones when `translucent`. */
void WorldScene::drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                          const CameraFrame& camera, bool opaque, bool translucent, f32 alpha,
                          std::optional<f32> textureFrameOffset) const {
    if (!unit.visible || unit.alpha <= 0.0f) {
        return;
    }
    const Mat4 world = camera.face(presentedWorldOf(unit.object, alpha), unit.facing);
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
        for (const u32 index : part.part->indices) {
            const MeshVertex& v = unit.mesh->vertices[index];
            const Vec3 normal = glm::normalize(normalMatrix * v.normal);
            const Vec4 placed = world * Vec4{v.position, 1.0f};
            Color color = shadeAt(part.additive, unit.prelit, v, Vec3{placed}, normal, m_lighting);
            if (unit.alpha < 1.0f) {
                color.a = static_cast<u8>(static_cast<f32>(color.a) * unit.alpha);
            }
            m_scratch.vertex(Vec3{placed}, color, unit.chrome ? chromeUv(normal) : v.uv,
                             v.lightmapUv * part.lightmapScale);
        }
        m_scratch.end();
        DrawState state;
        state.mipmaps = true;
        state.alphaToCoverage = true;
        state.blend = part.additive ? BlendMode::Additive : BlendMode::Alpha;
        state.lightmap = part.lightmap;
        state.uvOffset = slot.presentedOffset(textureFrameOffset);
        state.alphaTest = kAlphaTest; // Same native cutout test as the baked geometry.
        state.cullBack = true;
        state.depthWrite = unit.depthWrite && unit.alpha >= 1.0f;
        state.depthTest = unit.depthTest;
        state.darken = part.additive ? 0.0f : m_darken;
        // Fading a solid cutout must not opt it into soft flipbook blending.
        const auto cycleOffset =
            part.additive || !unit.depthWrite ? textureFrameOffset : std::nullopt;
        const Texture* texture = slot.presentedTexture(state, cycleOffset);
        device.draw(m_scratch, *texture, clip, state);
        if (slot.thermal && unit.depthTest && !unit.background) {
            constexpr f32 kTextureFramesPerSecond = 30.0f;
            const f32 seconds =
                (static_cast<f32>(m_textureFrame) + textureFrameOffset.value_or(0)) /
                kTextureFramesPerSecond;
            submitHeat(device, m_scratch, clip, camera, seconds);
        }
    }
}

void WorldScene::draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                      f32 presentationAlpha, std::optional<f32> textureFrameOffset) const {
    drawOpaque(device, clip, camera, presentationAlpha, textureFrameOffset);
    drawDeferred(device, clip, camera, presentationAlpha, textureFrameOffset);
}

void WorldScene::drawOpaque(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                            f32 presentationAlpha, std::optional<f32> textureFrameOffset) const {
    std::fill(m_worldValid.begin(), m_worldValid.end(), u8{0});
    std::fill(m_presentedValid.begin(), m_presentedValid.end(), u8{0});
    // Background sheets can intersect the arena in geometry space (A5's lightning
    // does). A bias within the deferred queue alone still composites them over
    // actors and pillars. Draw the authored depthless far layer before the solids.
    for (const Unit& unit : m_units) {
        if (unit.background) {
            drawUnit(device, unit, clip, camera, true, true, presentationAlpha, textureFrameOffset);
        }
    }
    usize next = 0;
    while (next < m_batches.size() && !m_batches[next].translucent && !m_batches[next].additive) {
        drawBatch(device, m_batches[next++], clip, textureFrameOffset);
    }
    // Moving objects' solid parts join the opaque; everything blended sorts by depth.
    for (const Unit& unit : m_units) {
        if (!unit.sorted && !unit.background) {
            drawUnit(device, unit, clip, camera, true, false, presentationAlpha,
                     textureFrameOffset);
        }
    }
}

void WorldScene::drawDeferred(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                              f32 presentationAlpha, std::optional<f32> textureFrameOffset) const {
    const Vec3& eye = camera.position;
    std::fill(m_worldValid.begin(), m_worldValid.end(), u8{0});
    std::fill(m_presentedValid.begin(), m_presentedValid.end(), u8{0});
    usize next = 0;
    while (next < m_batches.size() && !m_batches[next].translucent && !m_batches[next].additive) {
        ++next;
    }
    while (next < m_batches.size() && !m_batches[next].additive) {
        drawBatch(device, m_batches[next++], clip, textureFrameOffset);
    }
    // Larger view depths draw first. Negative authored biases defer overlays until after
    // ordinary translucent surfaces; depth testing still occludes them behind solid walls.
    m_order.resize(m_units.size());
    std::vector<f32> keys(m_units.size());
    for (usize i = 0; i < m_units.size(); ++i) {
        m_order[i] = i;
        const Vec3 origin{presentedWorldOf(m_units[i].object, presentationAlpha)[3]};
        keys[i] = glm::dot(origin - eye, camera.forward) + m_units[i].sortBias;
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](usize a, usize b) { return keys[a] > keys[b]; });
    for (const usize i : m_order) {
        const Unit& unit = m_units[i];
        if (!unit.background) {
            drawUnit(device, unit, clip, camera, unit.sorted, true, presentationAlpha,
                     textureFrameOffset);
        }
    }
    while (next < m_batches.size()) {
        drawBatch(device, m_batches[next++], clip, textureFrameOffset);
    }
}

} // namespace gdl
