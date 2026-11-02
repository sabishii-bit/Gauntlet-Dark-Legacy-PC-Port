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
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/WorldCollision.h"

namespace gdl {

namespace {

constexpr u32 kLightmapShift = 20U;
constexpr u64 kAdditiveKey = u64{1} << 40U;
constexpr u64 kNoDepthKey = u64{1} << 41U;
constexpr u64 kNoDepthTestKey = u64{1} << 42U;
constexpr u64 kVeilKey = u64{1} << 43U;

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
        if (std::abs(glm::dot(first, second)) > portable::cos(kHalfPi * 0.5f)) {
            result = glm::mat4_cast(portable::slerp(first, second, alpha));
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
                                      std::span<TextureSet* const> lenders,
                                      const ThermalMaterial& thermal) {
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
            slot.thermal = thermal.surface(entry.name);
            slot.usable = true;
            return slot;
        }
        log::warn("World scene: external texture {} ({}) was not lent; drawn white", index,
                  entry.name);
        slot.texture = &device.whiteTexture();
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
                       std::span<const usize> backgroundObjects,
                       std::span<const usize> cameraFadeObjects, const WorldCollision* floors,
                       const ThermalMaterial& thermal) {
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
        if (placement.moving) {
            m_movingPlacements.push_back(i);
        }
    }
    captureStaticPlacements();

    // Still geometry is keyed by texture and lightmap, the glows and the objects that keep
    // depth unwritten apart from the rest.
    std::unordered_map<u64, usize> batchByKey;
    // A veil is a solid seen through: its texture blends by its alpha instead of cutting
    // out, so it is drawn after the figures standing under it (the Trenches' haze).
    const auto veilOf = [&](u32 slot, bool additive) {
        return !additive && slot < textures.size() && textures.entry(slot).translucent();
    };
    // Not so a decal: a flat sheet lying on another object's floor (the tower's rune strips)
    // has nothing standing under it, and drawn after the pickups it would cut a hole where
    // a shadow wrote depth before it. It stays among the solids, as the original draws it.
    const auto decalOf = [&](const Mesh& mesh, const Vec3& offset, usize object) {
        if (floors == nullptr || mesh.vertices.empty()) {
            return false;
        }
        f32 lowest = std::numeric_limits<f32>::max();
        f32 highest = std::numeric_limits<f32>::lowest();
        for (const MeshVertex& v : mesh.vertices) {
            lowest = std::min(lowest, v.position.y + offset.y);
            highest = std::max(highest, v.position.y + offset.y);
        }
        if (highest - lowest > kDecalThickness) {
            return false;
        }
        const usize step = std::max<usize>(1, mesh.vertices.size() / 16);
        usize tried = 0;
        usize supported = 0;
        for (usize k = 0; k < mesh.vertices.size(); k += step) {
            const Vec3 at = mesh.vertices[k].position + offset;
            ++tried;
            auto hit = floors->floorAt(at + Vec3{0.0f, kDecalLift, 0.0f}, 0.0f, kDecalLift * 2);
            if (hit.has_value() && hit->object == static_cast<s32>(object)) {
                // Its own faces are the highest here: look under them.
                hit = floors->floorAt(at - Vec3{0.0f, 0.02f, 0.0f}, 0.0f, kDecalLift);
            }
            if (hit.has_value() && hit->object != static_cast<s32>(object) &&
                std::abs(hit->y - at.y) <= kDecalLift) {
                ++supported;
            }
        }
        return tried > 0 && supported * 2 >= tried;
    };
    const auto batchFor = [&](u32 slot, u32 lightmap, bool additive, bool depthWrite,
                              bool depthTest, bool veil) -> Batch& {
        const u64 key = u64{slot} | (u64{lightmap} << kLightmapShift) |
                        (additive ? kAdditiveKey : 0) | (depthWrite ? 0 : kNoDepthKey) |
                        (depthTest ? 0 : kNoDepthTestKey) | (veil ? kVeilKey : 0);
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
        batch.additive = additive;
        batch.depthWrite = depthWrite;
        batch.depthTest = depthTest;
        batch.veil = veil;
        batch.geometry.begin(PrimitiveTopology::TriangleList);
        m_batches.push_back(std::move(batch));
        batchByKey[key] = m_batches.size() - 1;
        return m_batches.back();
    };

    m_objectMeshes.assign(objects.size(), nullptr);
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
        m_objectMeshes[i] = mesh;
        const bool cameraFade = std::ranges::find(cameraFadeObjects, i) != cameraFadeObjects.end();
        // A presentation-only opt-in to MBSetupObject's 100-unit fade. Existing
        // long-distance fade flags, if present, retain their native precedence.
        const auto material =
            ObjectMaterial::fromFlags(object.objectFlags | (cameraFade ? 0x400U : 0U));
        const bool chrome = material.chrome;
        const bool additive = material.additive;
        // A mesh with a colour stream is shaded by it (the lightmapped geometry); an object
        // flagged to go without its lightmap is drawn flat, colours and all (pbSetDORegs).
        const bool flat = (object.objectFlags & object_flags::kNoLightmap) != 0;
        const bool prelit = mesh->prelit && !flat;
        const bool depthWrite = material.depthWrite;
        const u32 facing = material.facing;
        // The original skips the compare for the no-depth-test word. The camera-facing glows
        // flagged so keep it; a flat overlay lying on geometry (A3XOEXTRACTED_P, the Armory
        // rack's picture) would be painted through the walls in front of it, so it is tested
        // and lifted toward the eye instead.
        const bool lifted = !material.depthTest && facing == 0;
        const bool depthTest = material.depthTest || lifted;
        const bool background = std::ranges::find(backgroundObjects, i) != backgroundObjects.end();
        const bool burning = std::ranges::any_of(mesh->parts, [&](const MeshPart& part) {
            return slotFor(part.texture, textures, device, lenders, thermal).thermal;
        });
        const bool unit = burning || background || m_placements[i].moving || material.sorted ||
                          material.distanceFadeRange > 0 || facing != 0 || lifted ||
                          std::ranges::find(controlledObjects, i) != controlledObjects.end();
        Unit placedUnit;
        placedUnit.object = i;
        placedUnit.mesh = mesh;
        placedUnit.chrome = chrome;
        placedUnit.sorted = material.sorted;
        placedUnit.deferAlways = material.deferAlways;
        placedUnit.meshAlpha = mesh->alphaTextured();
        placedUnit.background = background;
        placedUnit.depthWrite = depthWrite;
        placedUnit.depthTest = depthTest;
        placedUnit.lifted = lifted;
        placedUnit.facing = facing;
        placedUnit.prelit = prelit;
        placedUnit.sortBias = material.sortBias;
        placedUnit.distanceFadeRange = material.distanceFadeRange;
        placedUnit.distanceSortScale = material.distanceSortScale;
        const Vec3 offset = layout.worldPosition(i);
        const bool decal = !m_placements[i].moving && decalOf(*mesh, offset, i);
        bool placed = false;
        for (const MeshPart& part : mesh->parts) {
            const Slot& slot = slotFor(part.texture, textures, device, lenders, thermal);
            if (!slot.usable) {
                continue;
            }
            try {
                const u32 lightmap = flat ? 0U : part.lightmap;
                if (unit) {
                    UnitPart unitPart;
                    unitPart.slot = part.texture;
                    unitPart.part = &part;
                    if (lightmap != 0 && lightmap < textures.size()) {
                        const TextureSetEntry& map = textures.entry(lightmap);
                        unitPart.lightmap = &textures.texture(device, lightmap);
                        unitPart.lightmapScale =
                            Vec2{1.0f / static_cast<f32>(std::max(map.width, 1U)),
                                 1.0f / static_cast<f32>(std::max(map.height, 1U))};
                    }
                    unitPart.additive = additive;
                    placedUnit.veil = placedUnit.veil || (veilOf(part.texture, additive) && !decal);
                    placedUnit.parts.push_back(unitPart);
                } else {
                    Batch& batch = batchFor(part.texture, lightmap, additive, depthWrite, depthTest,
                                            veilOf(part.texture, additive) && !decal);
                    for (const u32 index : part.indices) {
                        const MeshVertex& v = mesh->vertices[index];
                        const Vec3 at = v.position + offset;
                        batch.geometry.vertex(at, shadeOf(additive, prelit, v),
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
    // The solids first, the glows last so they add onto everything.
    std::stable_sort(m_batches.begin(), m_batches.end(), [](const Batch& a, const Batch& b) {
        return static_cast<int>(a.additive) < static_cast<int>(b.additive);
    });
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

WorldScene::StaticPlacements::StaticPlacements(std::vector<PlacementState> states)
    : values(std::move(states)) {
    NativeStateDigest digest;
    digest.string("WorldScene.static-placements/1");
    digest.value(values);
    fingerprint = digest.finish();
}

void WorldScene::captureStaticPlacements() {
    std::vector<PlacementState> values;
    values.reserve(m_placements.size() - m_movingPlacements.size());
    m_staticPreviousPending = false;
    for (const auto& placement : m_placements) {
        if (!placement.moving) {
            values.push_back(placement);
            m_staticPreviousPending |= placement.previous != placement.local;
        }
    }
    m_staticPlacements = std::make_shared<const StaticPlacements>(std::move(values));
}

const CanonicalDigestSink::Digest& WorldScene::rosterDigest() const {
    if (m_rosterDigest) {
        return *m_rosterDigest;
    }
    NativeStateDigest digest;
    digest.string("WorldScene.roster/1");
    digest.values(m_layoutSignature, m_movingPlacements);
    digest.wide(m_placements.size());
    for (const auto& placement : m_placements) {
        digest.values(placement.initial, placement.parent, placement.unit, placement.moving);
    }
    digest.wide(m_units.size());
    for (const auto& unit : m_units) {
        digest.values(unit.object, unit.sortBias, unit.distanceFadeRange, unit.distanceSortScale,
                      unit.facing, unit.prelit, unit.chrome, unit.sorted, unit.deferAlways,
                      unit.meshAlpha, unit.background, unit.depthWrite, unit.depthTest, unit.lifted,
                      unit.veil);
    }
    m_rosterDigest = digest.finish();
    return *m_rosterDigest;
}

bool WorldScene::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("WorldScene/2");
    // Build fixes the roster. Static poses are immutable versions, not a cache
    // of whichever version happens to be live when historical state is hashed.
    digest.bytes(rosterDigest());
    digest.bytes((saved ? saved->m_staticPlacements : m_staticPlacements)->fingerprint);
    digest.wide(m_movingPlacements.size());
    for (usize i = 0; i < m_movingPlacements.size(); ++i) {
        digest.value(saved
                         ? saved->m_placements[i]
                         : static_cast<const PlacementState&>(m_placements[m_movingPlacements[i]]));
    }
    digest.flag(saved ? saved->m_staticPreviousPending : m_staticPreviousPending);
    digest.wide(m_units.size());
    for (usize i = 0; i < m_units.size(); ++i) {
        digest.value(saved ? saved->m_units[i] : static_cast<const UnitState&>(m_units[i]));
    }
    // Slot iteration must not inherit unordered-map insertion/bucket order. Saved
    // motion is read directly, never selected from the live animation clock.
    std::vector<std::pair<u32, const SlotMotion*>> slots;
    slots.reserve(m_slots.size());
    if (saved) {
        for (const auto& [key, motion] : saved->m_slots) {
            slots.emplace_back(key, &motion);
        }
    } else {
        for (const auto& [key, motion] : m_slots) {
            slots.emplace_back(key, &motion);
        }
    }
    std::ranges::sort(slots, {}, &std::pair<u32, const SlotMotion*>::first);
    digest.wide(slots.size());
    for (const auto& [key, motion] : slots) {
        const auto found = m_slots.find(key);
        if (found == m_slots.end()) {
            digest.invalidate();
            return false;
        }
        digest.value(key);
        digest.resource(found->second.texture);
        digest.values(found->second.usable, found->second.thermal, *motion);
    }
    digest.values(saved ? saved->m_lighting : m_lighting, saved ? saved->m_darken : m_darken,
                  saved ? saved->m_textureFrame : m_textureFrame);
    // Bound mesh/batch geometry is immutable art authenticated at stage load.
    // Composed matrices, sorting, uploaded/lit vertices are derived draw scratch.
    return digest.good();
}

bool WorldScene::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool WorldScene::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [placements, staticPlacements, staticPreviousPending, units, slots,
                                  lighting, darken, textureFrame, owner, binding] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

void WorldScene::saveLocal(LocalSnapshot& saved) const {
    saved.m_placements.resize(m_movingPlacements.size());
    for (usize i = 0; i < m_movingPlacements.size(); ++i) {
        saved.m_placements[i] =
            static_cast<const PlacementState&>(m_placements[m_movingPlacements[i]]);
    }
    saved.m_staticPlacements = m_staticPlacements;
    saved.m_staticPreviousPending = m_staticPreviousPending;
    saved.m_units.resize(m_units.size());
    for (usize i = 0; i < m_units.size(); ++i) {
        saved.m_units[i] = static_cast<const UnitState&>(m_units[i]);
    }
    saved.m_slots.resize(m_slots.size());
    usize i = 0;
    for (const auto& [key, slot] : m_slots) {
        // Copy into the retained cycle buffer. Assigning a temporary pair
        // allocates a fresh cycle vector and throws away the ring slot's capacity.
        auto& destination = saved.m_slots[i++];
        destination.first = key;
        destination.second = slot;
    }
    saved.m_lighting = m_lighting;
    saved.m_darken = m_darken;
    saved.m_textureFrame = m_textureFrame;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
}

bool WorldScene::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_localBinding && saved.m_binding.lock() == m_localBinding &&
           saved.m_placements.size() == m_movingPlacements.size() && saved.m_staticPlacements &&
           saved.m_staticPlacements->values.size() ==
               m_placements.size() - m_movingPlacements.size() &&
           saved.m_units.size() == m_units.size() && saved.m_slots.size() == m_slots.size();
}

bool WorldScene::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) &&
           left.m_placements == right.m_placements &&
           left.m_staticPlacements->values == right.m_staticPlacements->values &&
           left.m_staticPreviousPending == right.m_staticPreviousPending &&
           left.m_units == right.m_units && left.m_slots == right.m_slots &&
           left.m_lighting == right.m_lighting && left.m_darken == right.m_darken &&
           left.m_textureFrame == right.m_textureFrame;
}

bool WorldScene::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < m_movingPlacements.size(); ++i) {
        static_cast<PlacementState&>(m_placements[m_movingPlacements[i]]) = saved.m_placements[i];
    }
    if (m_staticPlacements != saved.m_staticPlacements) {
        usize i = 0;
        for (auto& placement : m_placements) {
            if (!placement.moving) {
                static_cast<PlacementState&>(placement) = saved.m_staticPlacements->values[i++];
            }
        }
        m_staticPlacements = saved.m_staticPlacements;
    }
    m_staticPreviousPending = saved.m_staticPreviousPending;
    for (usize i = 0; i < m_units.size(); ++i) {
        static_cast<UnitState&>(m_units[i]) = saved.m_units[i];
    }
    for (const auto& [key, motion] : saved.m_slots) {
        static_cast<SlotMotion&>(m_slots.at(key)) = motion;
    }
    m_lighting = saved.m_lighting;
    m_darken = saved.m_darken;
    m_lighting.darken = m_darken;
    m_textureFrame = saved.m_textureFrame;
    std::fill(m_worldValid.begin(), m_worldValid.end(), u8{0});
    std::fill(m_presentedValid.begin(), m_presentedValid.end(), u8{0});
    return true;
}

void WorldScene::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_slots.clear();
    m_objectMeshes.clear();
    m_batches.clear();
    m_units.clear();
    m_placements.clear();
    m_movingPlacements.clear();
    m_staticPlacements = std::make_shared<const StaticPlacements>();
    m_rosterDigest.reset();
    m_staticPreviousPending = false;
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
    for (const usize object : m_movingPlacements) {
        auto& placement = m_placements[object];
        placement.previous = placement.local;
    }
    // The diagnostic restore can supply an earlier pose for a static parent.
    // It is settled once, just as in the full roster capture, without copying
    // the unchanged static world into every native history slot thereafter.
    if (m_staticPreviousPending) {
        for (auto& placement : m_placements) {
            if (!placement.moving) {
                placement.previous = placement.local;
            }
        }
        captureStaticPlacements();
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

const Mat4& WorldScene::localTransform(usize object) const {
    static const Mat4 kIdentity{1.0f};
    return object < m_placements.size() ? m_placements[object].local : kIdentity;
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
    bool staticChanged = false;
    for (const auto& object : state.objects) {
        auto& placement = m_placements[object.index];
        staticChanged |= !placement.moving &&
                         (placement.local != object.local || placement.previous != object.local ||
                          placement.continuity != object.continuity);
        placement.local = object.local;
        placement.previous = object.local;
        placement.continuity = object.continuity;
        if (auto* unit = unitOf(object.index)) {
            unit->alpha = object.alpha;
            unit->visible = object.visible;
        }
    }
    if (staticChanged) {
        captureStaticPlacements();
    }
    m_darken = state.darken;
    m_lighting.darken = m_darken;
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

void WorldScene::drawHighlight(RenderDevice& device, const Mat4& clip, usize object,
                               const SelectionHighlight& highlight) const {
    if (object >= m_objectMeshes.size() || m_objectMeshes[object] == nullptr) {
        return;
    }
    const Mesh& mesh = *m_objectMeshes[object];
    const Mat4& placement = worldTransform(object);
    for (const MeshPart& part : mesh.parts) {
        const auto slot = m_slots.find(part.texture);
        if (slot == m_slots.end() || !slot->second.usable || slot->second.current() == nullptr) {
            continue;
        }
        highlight.draw(device, clip, mesh, part, placement, *slot->second.current());
    }
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
        state.darken = 0.0f; // taken off the baked colour already, under the lights added
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

Color WorldScene::bakedShade(Color color) {
    constexpr u32 kFull = 248; ///< lbl_80348F54: the five-bit channels' top, spread to bytes
    const auto channel = [](u8 value) {
        return static_cast<u8>(std::min(255U, (u32{value} * 255U + kFull / 2) / kFull));
    };
    return Color{channel(color.r), channel(color.g), channel(color.b), color.a};
}

Color WorldScene::shadeOf(bool additive, bool prelit, const MeshVertex& vertex) {
    if (additive) {
        return kUnlit;
    }
    return prelit ? bakedShade(vertex.color) : kUnlit;
}

Color WorldScene::shadeAt(bool additive, bool prelit, const MeshVertex& vertex,
                          const Vec3& position, const Vec3& normal, const WorldLighting& lighting) {
    if (additive) {
        return kUnlit;
    }
    return lighting.brighten(shadeOf(additive, prelit, vertex), position, normal);
}

f32 WorldScene::drawnOpacity(const Unit& unit, const CameraFrame& camera,
                             f32 presentationAlpha) const {
    if (unit.distanceFadeRange <= 0) {
        return unit.alpha;
    }
    // MBSetupObject (800B9410) fades by the node origin's forward depth, not its
    // distance from the eye. Keep it draw-only: peers may use different cameras.
    const Vec3 origin{presentedWorldOf(unit.object, presentationAlpha)[3]};
    const f32 depth = glm::dot(origin - camera.position, camera.forward);
    const auto opacity =
        static_cast<u8>(std::clamp(depth * (255.0f / unit.distanceFadeRange), 0.0f, 255.0f));
    return unit.alpha * (static_cast<f32>(opacity) / 255.0f);
}

/** Places, lights and draws a unit's parts: the plain ones when `plainParts`, the glowing
 * ones when `additiveParts`. Gameplay fades stop depth writes; camera fades keep the mask. */
void WorldScene::drawUnit(RenderDevice& device, const Unit& unit, const Mat4& clip,
                          const CameraFrame& camera, bool plainParts, bool additiveParts, f32 alpha,
                          std::optional<f32> textureFrameOffset) const {
    const f32 opacity = drawnOpacity(unit, camera, alpha);
    if (!unit.visible || opacity <= 0.0f) {
        return;
    }
    const Mat4 world = camera.face(presentedWorldOf(unit.object, alpha), unit.facing);
    const Mat3 normalMatrix{world};
    for (const UnitPart& part : unit.parts) {
        if (part.additive ? !additiveParts : !plainParts) {
            continue;
        }
        const Slot& slot = m_slots.at(part.slot);
        m_scratch.clear();
        m_scratch.begin(PrimitiveTopology::TriangleList);
        for (const u32 index : part.part->indices) {
            const MeshVertex& v = unit.mesh->vertices[index];
            const Vec3 transformedNormal = normalMatrix * v.normal;
            const f32 normalLength = glm::length(transformedNormal);
            // Authored prelit meshes may have no normal; collapsed moving
            // geometry also yields zero. Preserve its baked colour and the
            // neutral chrome coordinate instead of normalizing zero to NaN.
            const Vec3 normal = normalLength > 0 && std::isfinite(normalLength)
                                    ? transformedNormal / normalLength
                                    : Vec3{0};
            Vec4 placed = world * Vec4{v.position, 1.0f};
            if (unit.lifted) {
                const Vec3 toEye = camera.position - Vec3{placed};
                const f32 reach = glm::length(toEye);
                if (reach > kOverlayLift) {
                    placed += Vec4{toEye * (kOverlayLift / reach), 0.0f};
                }
            }
            Color color = shadeAt(part.additive, unit.prelit, v, Vec3{placed}, normal, m_lighting);
            if (opacity < 1.0f) {
                color.a = static_cast<u8>(static_cast<f32>(color.a) * opacity);
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
        // MBSetupObject's camera fade changes alpha/queue, not the authored
        // depth mask. Explicit gameplay fades retain their existing policy.
        state.depthWrite = unit.depthWrite && unit.alpha >= 1.0f;
        state.depthTest = unit.depthTest;
        // Lit as it is placed (shadeAt): the darkening is in its colours, the lights over it.
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
    drawVeils(device, clip, camera, presentationAlpha, textureFrameOffset);
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
    while (next < m_batches.size() && !m_batches[next].additive) {
        if (!m_batches[next].veil) {
            drawBatch(device, m_batches[next], clip, textureFrameOffset);
        }
        ++next;
    }
    // Whatever the original draws at once joins the solids, cutouts included; only the
    // units it defers wait, the veils for the figures, and the glows add themselves after
    // everything.
    for (const Unit& unit : m_units) {
        if (!unit.background && !unit.deferred(drawnOpacity(unit, camera, presentationAlpha)) &&
            !unit.veil) {
            drawUnit(device, unit, clip, camera, true, false, presentationAlpha,
                     textureFrameOffset);
        }
    }
}

void WorldScene::drawVeils(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                           f32 presentationAlpha, std::optional<f32> textureFrameOffset) const {
    usize next = 0;
    while (next < m_batches.size() && !m_batches[next].additive) {
        if (m_batches[next].veil) {
            drawBatch(device, m_batches[next], clip, textureFrameOffset);
        }
        ++next;
    }
    for (const Unit& unit : m_units) {
        if (!unit.background && !unit.deferred(drawnOpacity(unit, camera, presentationAlpha)) &&
            unit.veil) {
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
    while (next < m_batches.size() && !m_batches[next].additive) {
        ++next;
    }
    // Larger view depths draw first. Negative authored biases defer overlays until after
    // ordinary translucent surfaces; depth testing still occludes them behind solid walls.
    m_order.resize(m_units.size());
    std::vector<f32> keys(m_units.size());
    for (usize i = 0; i < m_units.size(); ++i) {
        m_order[i] = i;
        const Vec3 origin{presentedWorldOf(m_units[i].object, presentationAlpha)[3]};
        const Unit& unit = m_units[i];
        const bool nearFade = unit.distanceFadeRange > 0 &&
                              drawnOpacity(unit, camera, presentationAlpha) < unit.alpha;
        const f32 depth = glm::dot(origin - eye, camera.forward);
        keys[i] = nearFade ? depth * unit.distanceSortScale : depth + unit.sortBias;
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](usize a, usize b) { return keys[a] > keys[b]; });
    for (const usize i : m_order) {
        const Unit& unit = m_units[i];
        if (!unit.background) {
            drawUnit(device, unit, clip, camera,
                     unit.deferred(drawnOpacity(unit, camera, presentationAlpha)), true,
                     presentationAlpha, textureFrameOffset);
        }
    }
    while (next < m_batches.size()) {
        drawBatch(device, m_batches[next++], clip, textureFrameOffset);
    }
}

} // namespace gdl
